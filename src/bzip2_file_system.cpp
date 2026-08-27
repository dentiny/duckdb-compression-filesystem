#include "bzip2_file_system.hpp"

#include "bzlib.h"
#include "compression_path_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cstring>

namespace duckdb {

namespace {

struct Bzip2StreamWrapper : public StreamWrapper {
	~Bzip2StreamWrapper() override {
		ReleaseState();
	}

	void Initialize(QueryContext context, CompressedFile &file, bool write) override;
	bool Read(StreamData &stream_data) override;
	void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes) override;
	void Close() override;
	void AbortWrite() override;

private:
	void FinishWrite();
	void ReleaseState();
	static void WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size);
	static const char *ErrorName(int code);

	CompressedFile *file = nullptr;
	bz_stream stream {};
	bool initialized = false;
	bool writing = false;
	bool draining_decoder_output = false;
};

void Bzip2StreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	file = &file_p;
	writing = write;
	memset(&stream, 0, sizeof(stream));

	const auto result = write ? BZ2_bzCompressInit(&stream, 9, 0, 0) : BZ2_bzDecompressInit(&stream, 0, 0);
	if (result != BZ_OK) {
		throw IOException("Failed to initialize bzip2 stream: %s", ErrorName(result));
	}
	initialized = true;
}

bool Bzip2StreamWrapper::Read(StreamData &sd) {
	D_ASSERT(initialized);
	D_ASSERT(!writing);

	const bool was_draining = draining_decoder_output;
	stream.next_in = was_draining ? nullptr : reinterpret_cast<char *>(sd.in_buff_start);
	stream.avail_in =
	    was_draining ? 0 : NumericCast<unsigned int>(UnsafeNumericCast<idx_t>(sd.in_buff_end - sd.in_buff_start));
	stream.next_out = reinterpret_cast<char *>(sd.out_buff.get());
	stream.avail_out = NumericCast<unsigned int>(sd.out_buf_size);

	const auto result = BZ2_bzDecompress(&stream);
	if (was_draining) {
		if (result == BZ_STREAM_END || stream.avail_out > 0) {
			draining_decoder_output = false;
			sd.in_buff_start = sd.in_buff_end;
		}
	} else {
		sd.in_buff_start = reinterpret_cast<data_ptr_t>(stream.next_in);
		if (result == BZ_OK && stream.avail_in == 0 && stream.avail_out == 0) {
			// CompressedFile stops invoking the decoder once its input buffer is
			// empty. Keep one consumed byte as a sentinel while bzip2 drains
			// output that it buffered internally; it is not passed in again.
			D_ASSERT(sd.in_buff_end > sd.in_buff.get());
			sd.in_buff_start = sd.in_buff_end - 1;
			draining_decoder_output = true;
		}
	}

	sd.out_buff_start = sd.out_buff.get();
	sd.out_buff_end = reinterpret_cast<data_ptr_t>(stream.next_out);

	if (result == BZ_STREAM_END) {
		return true;
	}
	if (result != BZ_OK) {
		throw IOException("Failed to decompress bzip2 stream: %s", ErrorName(result));
	}
	return false;
}

void Bzip2StreamWrapper::Write(CompressedFile &file_p, StreamData &sd, data_ptr_t buffer, int64_t nr_bytes) {
	D_ASSERT(initialized);
	D_ASSERT(writing);

	auto remaining = UnsafeNumericCast<idx_t>(nr_bytes);
	while (remaining > 0) {
		const auto input_size = MinValue<idx_t>(remaining, NumericLimits<unsigned int>::Maximum());
		stream.next_in = reinterpret_cast<char *>(buffer);
		stream.avail_in = NumericCast<unsigned int>(input_size);

		while (stream.avail_in > 0) {
			stream.next_out = reinterpret_cast<char *>(sd.out_buff.get());
			stream.avail_out = NumericCast<unsigned int>(sd.out_buf_size);
			const auto result = BZ2_bzCompress(&stream, BZ_RUN);
			if (result != BZ_RUN_OK) {
				throw IOException("Failed to compress bzip2 stream: %s", ErrorName(result));
			}
			WriteOutput(file_p, sd.out_buff.get(), sd.out_buf_size - stream.avail_out);
		}

		buffer += input_size;
		remaining -= input_size;
	}
}

void Bzip2StreamWrapper::FinishWrite() {
	D_ASSERT(file);
	D_ASSERT(initialized);
	D_ASSERT(writing);

	auto &sd = file->stream_data;
	stream.next_in = nullptr;
	stream.avail_in = 0;

	while (true) {
		stream.next_out = reinterpret_cast<char *>(sd.out_buff.get());
		stream.avail_out = NumericCast<unsigned int>(sd.out_buf_size);
		const auto result = BZ2_bzCompress(&stream, BZ_FINISH);
		WriteOutput(*file, sd.out_buff.get(), sd.out_buf_size - stream.avail_out);
		if (result == BZ_STREAM_END) {
			return;
		}
		if (result != BZ_FINISH_OK) {
			throw IOException("Failed to finish bzip2 stream: %s", ErrorName(result));
		}
	}
}

void Bzip2StreamWrapper::Close() {
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

void Bzip2StreamWrapper::AbortWrite() {
	ReleaseState();
	file = nullptr;
	writing = false;
}

void Bzip2StreamWrapper::ReleaseState() {
	draining_decoder_output = false;
	if (!initialized) {
		return;
	}
	if (writing) {
		BZ2_bzCompressEnd(&stream);
	} else {
		BZ2_bzDecompressEnd(&stream);
	}
	initialized = false;
}

void Bzip2StreamWrapper::WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size) {
	if (size > 0) {
		file.child_handle->Write(file.context, const_cast<data_ptr_t>(output), size);
	}
}

const char *Bzip2StreamWrapper::ErrorName(int code) {
	switch (code) {
	case BZ_SEQUENCE_ERROR:
		return "invalid operation sequence";
	case BZ_PARAM_ERROR:
		return "invalid parameter";
	case BZ_MEM_ERROR:
		return "memory allocation failure";
	case BZ_DATA_ERROR:
		return "corrupt compressed data";
	case BZ_DATA_ERROR_MAGIC:
		return "invalid bzip2 header";
	case BZ_IO_ERROR:
		return "I/O error";
	case BZ_UNEXPECTED_EOF:
		return "unexpected end of file";
	case BZ_OUTBUFF_FULL:
		return "output buffer full";
	case BZ_CONFIG_ERROR:
		return "library configuration error";
	default:
		return "unknown error";
	}
}

struct Bzip2FileSystemHolder {
	Bzip2FileSystem bzip2_fs;
};

class Bzip2File : private Bzip2FileSystemHolder, public CompressedFile {
public:
	Bzip2File(QueryContext context, unique_ptr<FileHandle> child_handle, const string &path, bool write)
	    : CompressedFile(bzip2_fs, std::move(child_handle), path) {
		Initialize(context, write);
	}

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(Bzip2FileSystem::COMPRESSION_NAME);
	}
};

} // namespace

unique_ptr<FileHandle> Bzip2FileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                           bool write) {
	auto path = handle->path;
	return make_uniq<Bzip2File>(context, std::move(handle), path, write);
}

bool Bzip2FileSystem::CanHandleFile(const string &fpath) {
	return CompressionPathUtils::HasExtension(fpath, ".bz2");
}

unique_ptr<StreamWrapper> Bzip2FileSystem::CreateStream() {
	return make_uniq<Bzip2StreamWrapper>();
}

idx_t Bzip2FileSystem::InBufferSize() {
	return idx_t(1) << 16;
}

idx_t Bzip2FileSystem::OutBufferSize() {
	return idx_t(1) << 16;
}

} // namespace duckdb
