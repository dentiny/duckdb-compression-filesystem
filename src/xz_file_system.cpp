#include "xz_file_system.hpp"

#include "compression_path_utils.hpp"
#include "lzma.h"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

namespace {

struct XzStreamWrapper : public StreamWrapper {
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

	const auto result = write ? lzma_easy_encoder(&stream, LZMA_PRESET_DEFAULT, LZMA_CHECK_CRC64)
	                          : lzma_stream_decoder(&stream, DECODER_MEMORY_LIMIT, 0);
	if (result != LZMA_OK) {
		throw IOException("Failed to initialize XZ stream: %s", ErrorName(result));
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
		return true;
	}
	if (result != LZMA_OK) {
		throw IOException("Failed to decompress XZ stream: %s", ErrorName(result));
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
			throw IOException("Failed to compress XZ stream: %s", ErrorName(result));
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
			throw IOException("Failed to finish XZ stream: %s", ErrorName(result));
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
		return "unrecognized XZ format";
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

struct XzFileSystemHolder {
	XzFileSystem xz_fs;
};

class XzFile : private XzFileSystemHolder, public CompressedFile {
public:
	XzFile(QueryContext context, unique_ptr<FileHandle> child_handle, const string &path, bool write)
	    : CompressedFile(xz_fs, std::move(child_handle), path) {
		Initialize(context, write);
	}

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(XzFileSystem::COMPRESSION_NAME);
	}
};

} // namespace

unique_ptr<FileHandle> XzFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                        bool write) {
	auto path = handle->path;
	return make_uniq<XzFile>(context, std::move(handle), path, write);
}

bool XzFileSystem::CanHandleFile(const string &fpath) {
	return CompressionPathUtils::HasExtension(fpath, ".xz");
}

unique_ptr<StreamWrapper> XzFileSystem::CreateStream() {
	return make_uniq<XzStreamWrapper>();
}

idx_t XzFileSystem::InBufferSize() {
	return idx_t(1) << 16;
}

idx_t XzFileSystem::OutBufferSize() {
	return idx_t(1) << 16;
}

} // namespace duckdb
