#include "brotli_file_system.hpp"

#include "brotli/decode.h"
#include "brotli/encode.h"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

struct BrotliStreamWrapper : public StreamWrapper {
	~BrotliStreamWrapper() override {
		DestroyStates();
	}

	void Initialize(QueryContext context, CompressedFile &file, bool write) override;
	bool Read(StreamData &stream_data) override;
	void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes) override;
	void Close() override;
	void AbortWrite() override;

private:
	void FinishWrite();
	void DestroyStates();
	static void WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size);

	CompressedFile *file = nullptr;
	duckdb_brotli::BrotliDecoderState *decoder = nullptr;
	duckdb_brotli::BrotliEncoderState *encoder = nullptr;
	bool writing = false;
	bool draining_decoder_output = false;
};

void BrotliStreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	file = &file_p;
	writing = write;
	if (write) {
		encoder = duckdb_brotli::BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
		if (!encoder) {
			throw IOException("Failed to initialize Brotli encoder");
		}
	} else {
		decoder = duckdb_brotli::BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
		if (!decoder) {
			throw IOException("Failed to initialize Brotli decoder");
		}
	}
}

bool BrotliStreamWrapper::Read(StreamData &sd) {
	D_ASSERT(!writing);
	D_ASSERT(decoder);

	const bool was_draining = draining_decoder_output;
	size_t available_input = was_draining ? 0 : UnsafeNumericCast<size_t>(sd.in_buff_end - sd.in_buff_start);
	const uint8_t *next_input = was_draining ? sd.in_buff_end : sd.in_buff_start;
	size_t available_output = UnsafeNumericCast<size_t>(sd.out_buf_size);
	uint8_t *next_output = sd.out_buff.get();

	const auto result = duckdb_brotli::BrotliDecoderDecompressStream(decoder, &available_input, &next_input,
	                                                                 &available_output, &next_output, nullptr);
	if (was_draining) {
		if (result != duckdb_brotli::BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
			draining_decoder_output = false;
			sd.in_buff_start = sd.in_buff_end;
		}
	} else {
		sd.in_buff_start = const_cast<data_ptr_t>(next_input);
		if (result == duckdb_brotli::BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT && available_input == 0) {
			// CompressedFile stops invoking the decoder once its input buffer is
			// empty. Keep one consumed byte as a sentinel while Brotli drains
			// output that it buffered internally; it is not passed in again.
			D_ASSERT(sd.in_buff_end > sd.in_buff.get());
			sd.in_buff_start = sd.in_buff_end - 1;
			draining_decoder_output = true;
		}
	}
	sd.out_buff_start = sd.out_buff.get();
	sd.out_buff_end = next_output;

	switch (result) {
	case duckdb_brotli::BROTLI_DECODER_RESULT_SUCCESS:
		return true;
	case duckdb_brotli::BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
		if (available_input != 0) {
			throw InternalException("Brotli decoder requested input without consuming its input buffer");
		}
		return false;
	case duckdb_brotli::BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
		return false;
	case duckdb_brotli::BROTLI_DECODER_RESULT_ERROR:
		throw IOException("Failed to decompress Brotli stream (error code %d)",
		                  duckdb_brotli::BrotliDecoderGetErrorCode(decoder));
	default:
		throw InternalException("Unexpected Brotli decoder result");
	}
}

void BrotliStreamWrapper::Write(CompressedFile &file_p, StreamData &sd, data_ptr_t buffer, int64_t nr_bytes) {
	D_ASSERT(writing);
	D_ASSERT(encoder);

	size_t available_input = UnsafeNumericCast<size_t>(nr_bytes);
	const uint8_t *next_input = buffer;
	do {
		size_t available_output = UnsafeNumericCast<size_t>(sd.out_buf_size);
		uint8_t *next_output = sd.out_buff.get();
		if (!duckdb_brotli::BrotliEncoderCompressStream(encoder, duckdb_brotli::BROTLI_OPERATION_PROCESS,
		                                                &available_input, &next_input, &available_output, &next_output,
		                                                nullptr)) {
			throw IOException("Failed to compress Brotli stream");
		}
		WriteOutput(file_p, sd.out_buff.get(), UnsafeNumericCast<idx_t>(next_output - sd.out_buff.get()));
	} while (available_input > 0 || duckdb_brotli::BrotliEncoderHasMoreOutput(encoder));
}

void BrotliStreamWrapper::FinishWrite() {
	D_ASSERT(file);
	D_ASSERT(encoder);

	while (!duckdb_brotli::BrotliEncoderIsFinished(encoder)) {
		size_t available_input = 0;
		const uint8_t *next_input = nullptr;
		auto &sd = file->stream_data;
		size_t available_output = UnsafeNumericCast<size_t>(sd.out_buf_size);
		uint8_t *next_output = sd.out_buff.get();
		if (!duckdb_brotli::BrotliEncoderCompressStream(encoder, duckdb_brotli::BROTLI_OPERATION_FINISH,
		                                                &available_input, &next_input, &available_output, &next_output,
		                                                nullptr)) {
			throw IOException("Failed to finish Brotli stream");
		}
		WriteOutput(*file, sd.out_buff.get(), UnsafeNumericCast<idx_t>(next_output - sd.out_buff.get()));
	}
}

void BrotliStreamWrapper::Close() {
	if (!file) {
		return;
	}
	if (writing) {
		FinishWrite();
	}
	DestroyStates();
	file = nullptr;
	writing = false;
}

void BrotliStreamWrapper::AbortWrite() {
	DestroyStates();
	file = nullptr;
	writing = false;
}

void BrotliStreamWrapper::DestroyStates() {
	draining_decoder_output = false;
	if (decoder) {
		duckdb_brotli::BrotliDecoderDestroyInstance(decoder);
		decoder = nullptr;
	}
	if (encoder) {
		duckdb_brotli::BrotliEncoderDestroyInstance(encoder);
		encoder = nullptr;
	}
}

void BrotliStreamWrapper::WriteOutput(CompressedFile &file, const_data_ptr_t output, idx_t size) {
	if (size > 0) {
		file.child_handle->Write(file.context, const_cast<data_ptr_t>(output), size);
	}
}

struct BrotliFileSystemHolder {
	BrotliFileSystem brotli_fs;
};

class BrotliFile : private BrotliFileSystemHolder, public CompressedFile {
public:
	BrotliFile(QueryContext context, unique_ptr<FileHandle> child_handle, const string &path, bool write)
	    : CompressedFile(brotli_fs, std::move(child_handle), path) {
		Initialize(context, write);
	}

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(BrotliFileSystem::COMPRESSION_NAME);
	}
};

} // namespace

unique_ptr<FileHandle> BrotliFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                            bool write) {
	auto path = handle->path;
	return make_uniq<BrotliFile>(context, std::move(handle), path, write);
}

bool BrotliFileSystem::CanHandleFile(const string &fpath) {
	auto path = fpath;
	if (!StringUtil::StartsWith(path, "\\\\?\\")) {
		const auto question_mark_pos = path.find('?');
		path = path.substr(0, question_mark_pos);
	}
	return StringUtil::EndsWith(StringUtil::Lower(path), ".br");
}

unique_ptr<StreamWrapper> BrotliFileSystem::CreateStream() {
	return make_uniq<BrotliStreamWrapper>();
}

idx_t BrotliFileSystem::InBufferSize() {
	return idx_t(1) << 16;
}

idx_t BrotliFileSystem::OutBufferSize() {
	return idx_t(1) << 16;
}

} // namespace duckdb
