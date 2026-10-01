#include "xz_file_system.hpp"

#include "compression_path_utils.hpp"
#include "lzma.h"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

namespace {

struct XzFileSystemHolder {
	explicit XzFileSystemHolder(XzFileSystem::Container container) : xz_fs(container) {
	}

	XzFileSystem xz_fs;
};

class XzFile : private XzFileSystemHolder, public CompressedFile {
public:
	XzFile(QueryContext context, unique_ptr<FileHandle> child_handle, const string &path, bool write,
	       XzFileSystem::Container container)
	    : XzFileSystemHolder(container), CompressedFile(xz_fs, std::move(child_handle), path) {
		Initialize(context, write);
	}

	bool read_finished = false;

	FileCompressionType GetFileCompressionType() override {
		return compressed_fs.GetCompressionType();
	}
};

struct XzStreamWrapper : public StreamWrapper {
	explicit XzStreamWrapper(XzFileSystem::Container container) : container(container) {
	}

	~XzStreamWrapper() override {
		ReleaseState();
	}

	void Initialize(QueryContext context, CompressedFile &file, bool write) override;
	bool Read(StreamData &stream_data) override;
	void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes) override;
	void Close() override;
	void AbortWrite() override;

private:
	static constexpr uint64_t DECODER_MEMORY_LIMIT = uint64_t(1) << 30;

	void FinishWrite();
	void ReleaseState();
	static void WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size);
	static const char *ErrorName(lzma_ret code);

	const char *FormatName() const {
		return container == XzFileSystem::Container::XZ ? "XZ" : "LZMA";
	}

	XzFileSystem::Container container;
	bool *read_finished = nullptr;
	CompressedFile *file = nullptr;
	lzma_stream stream = LZMA_STREAM_INIT;
	bool initialized = false;
	bool writing = false;
	bool draining_decoder_output = false;
};

void XzStreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	file = &file_p;
	writing = write;
	stream = LZMA_STREAM_INIT;

	read_finished = &file_p.Cast<XzFile>().read_finished;
	*read_finished = false;
	lzma_ret result;
	if (container == XzFileSystem::Container::LZMA_ALONE) {
		if (write) {
			lzma_options_lzma options {};
			if (lzma_lzma_preset(&options, LZMA_PRESET_DEFAULT)) {
				throw InternalException("Invalid default LZMA preset");
			}
			result = lzma_alone_encoder(&stream, &options);
		} else {
			result = lzma_alone_decoder(&stream, DECODER_MEMORY_LIMIT);
		}
	} else {
		result = write ? lzma_easy_encoder(&stream, LZMA_PRESET_DEFAULT, LZMA_CHECK_CRC64)
		               : lzma_stream_decoder(&stream, DECODER_MEMORY_LIMIT, 0);
	}
	if (result != LZMA_OK) {
		throw IOException("Failed to initialize %s stream: %s", FormatName(), ErrorName(result));
	}
	initialized = true;
}

bool XzStreamWrapper::Read(StreamData &sd) {
	D_ASSERT(initialized);
	D_ASSERT(!writing);

	const bool was_draining = draining_decoder_output;
	stream.next_in = was_draining ? nullptr : sd.in_buff_start;
	stream.avail_in = was_draining ? 0 : UnsafeNumericCast<size_t>(sd.in_buff_end - sd.in_buff_start);
	stream.next_out = sd.out_buff.get();
	stream.avail_out = UnsafeNumericCast<size_t>(sd.out_buf_size);

	const auto result = lzma_code(&stream, LZMA_RUN);
	if (was_draining) {
		if (result == LZMA_STREAM_END || stream.avail_out > 0) {
			draining_decoder_output = false;
			sd.in_buff_start = sd.in_buff_end;
		}
	} else {
		sd.in_buff_start = const_cast<data_ptr_t>(stream.next_in);
		if (result == LZMA_OK && stream.avail_in == 0 && stream.avail_out == 0) {
			// CompressedFile stops invoking the decoder once its input buffer is
			// empty. Keep one consumed byte as a sentinel while liblzma drains
			// output that it buffered internally; it is not passed in again.
			D_ASSERT(sd.in_buff_end > sd.in_buff.get());
			sd.in_buff_start = sd.in_buff_end - 1;
			draining_decoder_output = true;
		}
	}

	sd.out_buff_start = sd.out_buff.get();
	sd.out_buff_end = stream.next_out;

	if (result == LZMA_STREAM_END) {
		*read_finished = true;
		return true;
	}
	if (result != LZMA_OK) {
		throw IOException("Failed to decompress %s stream: %s", FormatName(), ErrorName(result));
	}
	return false;
}

void XzStreamWrapper::Write(CompressedFile &file_p, StreamData &sd, data_ptr_t buffer, int64_t nr_bytes) {
	D_ASSERT(initialized);
	D_ASSERT(writing);

	stream.next_in = buffer;
	stream.avail_in = UnsafeNumericCast<size_t>(nr_bytes);
	while (stream.avail_in > 0) {
		stream.next_out = sd.out_buff.get();
		stream.avail_out = UnsafeNumericCast<size_t>(sd.out_buf_size);
		const auto result = lzma_code(&stream, LZMA_RUN);
		if (result != LZMA_OK) {
			throw IOException("Failed to compress %s stream: %s", FormatName(), ErrorName(result));
		}
		WriteOutput(file_p, sd.out_buff.get(), sd.out_buf_size - stream.avail_out);
	}
}

void XzStreamWrapper::FinishWrite() {
	D_ASSERT(file);
	D_ASSERT(initialized);
	D_ASSERT(writing);

	auto &sd = file->stream_data;
	stream.next_in = nullptr;
	stream.avail_in = 0;

	while (true) {
		stream.next_out = sd.out_buff.get();
		stream.avail_out = UnsafeNumericCast<size_t>(sd.out_buf_size);
		const auto result = lzma_code(&stream, LZMA_FINISH);
		WriteOutput(*file, sd.out_buff.get(), sd.out_buf_size - stream.avail_out);
		if (result == LZMA_STREAM_END) {
			return;
		}
		if (result != LZMA_OK) {
			throw IOException("Failed to finish %s stream: %s", FormatName(), ErrorName(result));
		}
	}
}

void XzStreamWrapper::Close() {
	if (!initialized) {
		return;
	}
	if (writing) {
		FinishWrite();
	}
	ReleaseState();
	file = nullptr;
	writing = false;
}

void XzStreamWrapper::AbortWrite() {
	ReleaseState();
	file = nullptr;
	writing = false;
}

void XzStreamWrapper::ReleaseState() {
	draining_decoder_output = false;
	if (!initialized) {
		return;
	}
	lzma_end(&stream);
	stream = LZMA_STREAM_INIT;
	initialized = false;
}

void XzStreamWrapper::WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size) {
	if (size > 0) {
		file.child_handle->Write(file.context, const_cast<data_ptr_t>(output), size);
	}
}

const char *XzStreamWrapper::ErrorName(lzma_ret code) {
	switch (code) {
	case LZMA_MEM_ERROR:
		return "memory allocation failure";
	case LZMA_MEMLIMIT_ERROR:
		return "decoder memory limit exceeded";
	case LZMA_FORMAT_ERROR:
		return "unrecognized stream format";
	case LZMA_OPTIONS_ERROR:
		return "unsupported compression options";
	case LZMA_DATA_ERROR:
		return "corrupt compressed data";
	case LZMA_BUF_ERROR:
		return "truncated or incomplete stream";
	case LZMA_PROG_ERROR:
		return "invalid liblzma API usage";
	case LZMA_UNSUPPORTED_CHECK:
		return "unsupported integrity check";
	default:
		return "unknown error";
	}
}

} // namespace

unique_ptr<FileHandle> XzFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                        bool write) {
	auto path = handle->path;
	return make_uniq<XzFile>(context, std::move(handle), path, write, container);
}

int64_t XzFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	const auto result = CompressedFileSystem::Read(handle, buffer, nr_bytes);
	// The pinned CompressedFile API does not notify StreamWrapper of physical EOF.
	if (result < nr_bytes && !handle.Cast<XzFile>().read_finished) {
		throw IOException("Truncated %s stream", container == Container::XZ ? "XZ" : "LZMA");
	}
	return result;
}

bool XzFileSystem::CanHandleFile(const string &fpath) {
	return CompressionPathUtils::HasExtension(fpath, container == Container::XZ ? ".xz" : ".lzma");
}

unique_ptr<StreamWrapper> XzFileSystem::CreateStream() {
	return make_uniq<XzStreamWrapper>(container);
}

idx_t XzFileSystem::InBufferSize() {
	return idx_t(1) << 16;
}

idx_t XzFileSystem::OutBufferSize() {
	return idx_t(1) << 16;
}

} // namespace duckdb
