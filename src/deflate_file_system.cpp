#include "deflate_file_system.hpp"

#include "compression_path_utils.hpp"
#include "miniz.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cstring>

namespace duckdb {

namespace {

struct DeflateFileSystemHolder {
	DeflateFileSystem deflate_fs;
};

class DeflateFile : private DeflateFileSystemHolder, public CompressedFile {
public:
	DeflateFile(QueryContext context, unique_ptr<FileHandle> child_handle, const string &path, bool write)
	    : CompressedFile(deflate_fs, std::move(child_handle), path) {
		Initialize(context, write);
	}

	bool read_finished = false;

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(DeflateFileSystem::COMPRESSION_NAME);
	}
};

struct DeflateStreamWrapper : public StreamWrapper {
	~DeflateStreamWrapper() override {
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

	CompressedFile *file = nullptr;
	duckdb_miniz::mz_stream stream {};
	bool *read_finished = nullptr;
	bool initialized = false;
	bool writing = false;
	bool draining_decoder_output = false;
};

void DeflateStreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	file = &file_p;
	writing = write;
	memset(&stream, 0, sizeof(stream));

	read_finished = &file_p.Cast<DeflateFile>().read_finished;
	*read_finished = false;
	// Positive window bits (the default) select the RFC 1950 zlib wrapper,
	// including the Adler-32 checksum, as used by Hadoop DefaultCodec.
	const auto result = write ? duckdb_miniz::mz_deflateInit(&stream, duckdb_miniz::MZ_DEFAULT_COMPRESSION)
	                          : duckdb_miniz::mz_inflateInit(&stream);
	if (result != duckdb_miniz::MZ_OK) {
		throw IOException("Failed to initialize deflate stream: %s", duckdb_miniz::mz_error(result));
	}
	initialized = true;
}

bool DeflateStreamWrapper::Read(StreamData &sd) {
	D_ASSERT(initialized);
	D_ASSERT(!writing);

	const bool was_draining = draining_decoder_output;
	stream.next_in = was_draining ? nullptr : sd.in_buff_start;
	stream.avail_in =
	    was_draining ? 0 : NumericCast<unsigned int>(UnsafeNumericCast<idx_t>(sd.in_buff_end - sd.in_buff_start));
	stream.next_out = sd.out_buff.get();
	stream.avail_out = NumericCast<unsigned int>(sd.out_buf_size);

	const auto result = duckdb_miniz::mz_inflate(&stream, duckdb_miniz::MZ_NO_FLUSH);
	if (was_draining) {
		if (result == duckdb_miniz::MZ_STREAM_END || stream.avail_out > 0) {
			draining_decoder_output = false;
			sd.in_buff_start = sd.in_buff_end;
		}
	} else {
		sd.in_buff_start = const_cast<data_ptr_t>(stream.next_in);
		if (result == duckdb_miniz::MZ_OK && stream.avail_in == 0 && stream.avail_out == 0) {
			// CompressedFile stops invoking the decoder once its input buffer is
			// empty. Keep one consumed byte as a sentinel while miniz drains
			// output that it buffered internally; it is not passed in again.
			D_ASSERT(sd.in_buff_end > sd.in_buff.get());
			sd.in_buff_start = sd.in_buff_end - 1;
			draining_decoder_output = true;
		}
	}

	sd.out_buff_start = sd.out_buff.get();
	sd.out_buff_end = stream.next_out;

	if (result == duckdb_miniz::MZ_STREAM_END) {
		*read_finished = true;
		return true;
	}
	if (result != duckdb_miniz::MZ_OK && !(was_draining && result == duckdb_miniz::MZ_BUF_ERROR)) {
		throw IOException("Failed to decompress deflate stream: %s", duckdb_miniz::mz_error(result));
	}
	return false;
}

void DeflateStreamWrapper::Write(CompressedFile &file_p, StreamData &sd, data_ptr_t buffer, int64_t nr_bytes) {
	D_ASSERT(initialized);
	D_ASSERT(writing);

	auto remaining = UnsafeNumericCast<idx_t>(nr_bytes);
	while (remaining > 0) {
		const auto input_size = MinValue<idx_t>(remaining, NumericLimits<unsigned int>::Maximum());
		stream.next_in = buffer;
		stream.avail_in = NumericCast<unsigned int>(input_size);

		while (stream.avail_in > 0) {
			stream.next_out = sd.out_buff.get();
			stream.avail_out = NumericCast<unsigned int>(sd.out_buf_size);
			const auto result = duckdb_miniz::mz_deflate(&stream, duckdb_miniz::MZ_NO_FLUSH);
			if (result != duckdb_miniz::MZ_OK) {
				throw IOException("Failed to compress deflate stream: %s", duckdb_miniz::mz_error(result));
			}
			WriteOutput(file_p, sd.out_buff.get(), sd.out_buf_size - stream.avail_out);
		}

		buffer += input_size;
		remaining -= input_size;
	}
}

void DeflateStreamWrapper::FinishWrite() {
	D_ASSERT(file);
	D_ASSERT(initialized);
	D_ASSERT(writing);

	auto &sd = file->stream_data;
	stream.next_in = nullptr;
	stream.avail_in = 0;

	while (true) {
		stream.next_out = sd.out_buff.get();
		stream.avail_out = NumericCast<unsigned int>(sd.out_buf_size);
		const auto result = duckdb_miniz::mz_deflate(&stream, duckdb_miniz::MZ_FINISH);
		WriteOutput(*file, sd.out_buff.get(), sd.out_buf_size - stream.avail_out);
		if (result == duckdb_miniz::MZ_STREAM_END) {
			return;
		}
		if (result != duckdb_miniz::MZ_OK) {
			throw IOException("Failed to finish deflate stream: %s", duckdb_miniz::mz_error(result));
		}
	}
}

void DeflateStreamWrapper::Close() {
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

void DeflateStreamWrapper::AbortWrite() {
	ReleaseState();
	file = nullptr;
	writing = false;
}

void DeflateStreamWrapper::ReleaseState() {
	draining_decoder_output = false;
	if (!initialized) {
		return;
	}
	if (writing) {
		duckdb_miniz::mz_deflateEnd(&stream);
	} else {
		duckdb_miniz::mz_inflateEnd(&stream);
	}
	initialized = false;
}

void DeflateStreamWrapper::WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size) {
	if (size > 0) {
		file.child_handle->Write(file.context, const_cast<data_ptr_t>(output), size);
	}
}

} // namespace

unique_ptr<FileHandle> DeflateFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                             bool write) {
	auto path = handle->path;
	return make_uniq<DeflateFile>(context, std::move(handle), path, write);
}

int64_t DeflateFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &file = handle.Cast<DeflateFile>();
	const auto result = CompressedFileSystem::Read(handle, buffer, nr_bytes);
	// CompressedFile does not notify StreamWrapper on physical EOF in the
	// pinned DuckDB version. A short read must have reached the zlib trailer.
	if (result < nr_bytes && !file.read_finished) {
		throw IOException("Truncated deflate stream: missing end of stream or zlib checksum");
	}
	return result;
}

bool DeflateFileSystem::CanHandleFile(const string &fpath) {
	return CompressionPathUtils::HasExtension(fpath, ".deflate");
}

unique_ptr<StreamWrapper> DeflateFileSystem::CreateStream() {
	return make_uniq<DeflateStreamWrapper>();
}

idx_t DeflateFileSystem::InBufferSize() {
	return idx_t(1) << 16;
}

idx_t DeflateFileSystem::OutBufferSize() {
	return idx_t(1) << 16;
}

} // namespace duckdb
