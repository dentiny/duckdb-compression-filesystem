#include "crc32c.hpp"
#include "compression_path_utils.hpp"
#include "snappy_file_system.hpp"
#include "snappy_frame_utils.hpp"

#include "snappy.h"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cstring>

namespace duckdb {

namespace {

struct SnappyStreamWrapper : public StreamWrapper {
	~SnappyStreamWrapper() override = default;

	void Initialize(QueryContext context, CompressedFile &file, bool write) override;
	bool Read(StreamData &stream_data) override;
	void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes) override;
	void Close() override;
	void AbortWrite() override;

private:
	void WriteStreamIdentifier(CompressedFile &file);
	void FlushPendingChunk(CompressedFile &file);
	static void WriteChild(CompressedFile &file, const void *data, idx_t size);
	static void VerifyChecksum(uint32_t expected, const_data_ptr_t data, idx_t size);

	CompressedFile *file = nullptr;
	bool writing = false;
	bool stream_identifier_seen = false;
	idx_t skippable_bytes_remaining = 0;
	unsafe_unique_array<data_t> pending;
	idx_t pending_size = 0;
};

void SnappyStreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	file = &file_p;
	writing = write;
	stream_identifier_seen = false;
	skippable_bytes_remaining = 0;
	pending_size = 0;
	if (write) {
		pending = make_unsafe_uniq_array<data_t>(SnappyFrame::MAX_UNCOMPRESSED_CHUNK_SIZE);
		WriteStreamIdentifier(file_p);
	}
}

void SnappyStreamWrapper::WriteStreamIdentifier(CompressedFile &file_p) {
	static constexpr data_t STREAM_IDENTIFIER[] = {
	    SnappyFrame::STREAM_IDENTIFIER_CHUNK, 0x06, 0x00, 0x00, 's', 'N', 'a', 'P', 'p', 'Y'};
	WriteChild(file_p, STREAM_IDENTIFIER, sizeof(STREAM_IDENTIFIER));
}

void SnappyStreamWrapper::VerifyChecksum(uint32_t expected, const_data_ptr_t data, idx_t size) {
	if (expected != CRC32C::ComputeMasked(data, size)) {
		throw IOException("Snappy framed stream checksum mismatch");
	}
}

bool SnappyStreamWrapper::Read(StreamData &sd) {
	D_ASSERT(!writing);
	SnappyInputCursor input(sd);

	while (true) {
		if (skippable_bytes_remaining > 0) {
			const auto skip_size = MinValue<idx_t>(input.Available(), skippable_bytes_remaining);
			input.Consume(skip_size);
			skippable_bytes_remaining -= skip_size;
			if (skippable_bytes_remaining > 0) {
				return false;
			}
			continue;
		}

		if (input.NeedsMore(SnappyFrame::CHUNK_HEADER_SIZE, "chunk header")) {
			return false;
		}

		const auto *header = input.Current();
		const auto chunk_type = header[0];
		const auto payload_size = UnsafeNumericCast<idx_t>(SnappyFrame::LoadLittleEndian24(header + 1));

		if (!stream_identifier_seen && chunk_type != SnappyFrame::STREAM_IDENTIFIER_CHUNK) {
			throw IOException("Snappy framed stream does not begin with the stream identifier");
		}

		if (chunk_type == SnappyFrame::STREAM_IDENTIFIER_CHUNK) {
			if (payload_size != 6) {
				throw IOException("Invalid Snappy framed stream identifier");
			}
			const auto chunk_size = SnappyFrame::CHUNK_HEADER_SIZE + payload_size;
			if (input.NeedsMore(chunk_size, "stream identifier")) {
				return false;
			}
			const auto *payload = header + SnappyFrame::CHUNK_HEADER_SIZE;
			if (!SnappyFrame::IsStreamIdentifier(payload, payload_size)) {
				throw IOException("Invalid Snappy framed stream identifier");
			}
			stream_identifier_seen = true;
			input.Consume(chunk_size);
			continue;
		}

		if (SnappyFrame::IsSkippableChunk(chunk_type)) {
			input.Consume(SnappyFrame::CHUNK_HEADER_SIZE);
			skippable_bytes_remaining = payload_size;
			continue;
		}
		if (chunk_type > SnappyFrame::UNCOMPRESSED_CHUNK && chunk_type <= SnappyFrame::LAST_UNSKIPPABLE_CHUNK) {
			throw IOException("Unsupported unskippable Snappy chunk type 0x%02x", chunk_type);
		}
		if (chunk_type != SnappyFrame::COMPRESSED_CHUNK && chunk_type != SnappyFrame::UNCOMPRESSED_CHUNK) {
			throw InternalException("Unexpected Snappy chunk type");
		}
		if (payload_size < SnappyFrame::CHECKSUM_SIZE) {
			throw IOException("Invalid Snappy data chunk");
		}

		const auto chunk_size = SnappyFrame::CHUNK_HEADER_SIZE + payload_size;
		if (input.NeedsMore(chunk_size, "data chunk")) {
			return false;
		}
		const auto *payload = header + SnappyFrame::CHUNK_HEADER_SIZE;

		if (chunk_type == SnappyFrame::COMPRESSED_CHUNK) {
			const auto expected_checksum = SnappyFrame::LoadLittleEndian32(payload);
			const auto *compressed = payload + SnappyFrame::CHECKSUM_SIZE;
			const auto compressed_size = payload_size - SnappyFrame::CHECKSUM_SIZE;
			size_t uncompressed_size;
			if (!duckdb_snappy::GetUncompressedLength(const_char_ptr_cast(compressed), compressed_size,
			                                          &uncompressed_size)) {
				throw IOException("Invalid compressed Snappy chunk");
			}
			if (uncompressed_size > SnappyFrame::MAX_UNCOMPRESSED_CHUNK_SIZE || uncompressed_size > sd.out_buf_size) {
				throw IOException("Snappy chunk exceeds the maximum uncompressed size");
			}
			auto *output = sd.out_buff.get();
			if (!duckdb_snappy::RawUncompress(const_char_ptr_cast(compressed), compressed_size,
			                                  char_ptr_cast(output))) {
				throw IOException("Failed to decompress Snappy chunk");
			}
			VerifyChecksum(expected_checksum, output, uncompressed_size);
			sd.out_buff_start = output;
			sd.out_buff_end = output + uncompressed_size;
			input.Consume(chunk_size);
			return false;
		}

		if (chunk_type == SnappyFrame::UNCOMPRESSED_CHUNK) {
			const auto expected_checksum = SnappyFrame::LoadLittleEndian32(payload);
			const auto *uncompressed = payload + SnappyFrame::CHECKSUM_SIZE;
			const auto uncompressed_size = payload_size - SnappyFrame::CHECKSUM_SIZE;
			if (uncompressed_size > SnappyFrame::MAX_UNCOMPRESSED_CHUNK_SIZE || uncompressed_size > sd.out_buf_size) {
				throw IOException("Snappy chunk exceeds the maximum uncompressed size");
			}
			VerifyChecksum(expected_checksum, uncompressed, uncompressed_size);
			memcpy(sd.out_buff.get(), uncompressed, uncompressed_size);
			sd.out_buff_start = sd.out_buff.get();
			sd.out_buff_end = sd.out_buff.get() + uncompressed_size;
			input.Consume(chunk_size);
			return false;
		}
	}
}

void SnappyStreamWrapper::FlushPendingChunk(CompressedFile &file_p) {
	if (pending_size == 0) {
		return;
	}

	const auto maximum_compressed_size = UnsafeNumericCast<idx_t>(duckdb_snappy::MaxCompressedLength(pending_size));
	auto compressed = make_unsafe_uniq_array<data_t>(maximum_compressed_size);
	size_t compressed_size;
	duckdb_snappy::RawCompress(const_char_ptr_cast(pending.get()), pending_size, char_ptr_cast(compressed.get()),
	                           &compressed_size);

	const bool use_compressed = compressed_size < pending_size;
	const auto payload_size =
	    SnappyFrame::CHECKSUM_SIZE + (use_compressed ? UnsafeNumericCast<idx_t>(compressed_size) : pending_size);
	data_t header[SnappyFrame::CHUNK_HEADER_SIZE];
	header[0] = use_compressed ? SnappyFrame::COMPRESSED_CHUNK : SnappyFrame::UNCOMPRESSED_CHUNK;
	SnappyFrame::StoreLittleEndian24(header + 1, UnsafeNumericCast<uint32_t>(payload_size));

	data_t checksum[SnappyFrame::CHECKSUM_SIZE];
	SnappyFrame::StoreLittleEndian32(checksum, CRC32C::ComputeMasked(pending.get(), pending_size));

	WriteChild(file_p, header, sizeof(header));
	WriteChild(file_p, checksum, sizeof(checksum));
	WriteChild(file_p, use_compressed ? compressed.get() : pending.get(),
	           use_compressed ? UnsafeNumericCast<idx_t>(compressed_size) : pending_size);
	pending_size = 0;
}

void SnappyStreamWrapper::Write(CompressedFile &file_p, StreamData &, data_ptr_t buffer, int64_t nr_bytes) {
	D_ASSERT(writing);
	auto remaining = UnsafeNumericCast<idx_t>(nr_bytes);
	auto *source = buffer;
	while (remaining > 0) {
		const auto available = SnappyFrame::MAX_UNCOMPRESSED_CHUNK_SIZE - pending_size;
		const auto copy_size = MinValue<idx_t>(available, remaining);
		memcpy(pending.get() + pending_size, source, copy_size);
		pending_size += copy_size;
		source += copy_size;
		remaining -= copy_size;
		if (pending_size == SnappyFrame::MAX_UNCOMPRESSED_CHUNK_SIZE) {
			FlushPendingChunk(file_p);
		}
	}
}

void SnappyStreamWrapper::Close() {
	if (!file) {
		return;
	}
	if (writing) {
		FlushPendingChunk(*file);
	}
	AbortWrite();
}

void SnappyStreamWrapper::AbortWrite() {
	pending.reset();
	pending_size = 0;
	file = nullptr;
	writing = false;
	stream_identifier_seen = false;
	skippable_bytes_remaining = 0;
}

void SnappyStreamWrapper::WriteChild(CompressedFile &file, const void *data, idx_t size) {
	file.child_handle->Write(file.context, const_cast<void *>(data), size);
}

struct SnappyFileSystemHolder {
	SnappyFileSystem snappy_fs;
};

class SnappyFile : private SnappyFileSystemHolder, public CompressedFile {
public:
	SnappyFile(QueryContext context, unique_ptr<FileHandle> child_handle, const string &path, bool write)
	    : CompressedFile(snappy_fs, std::move(child_handle), path) {
		Initialize(context, write);
	}

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(SnappyFileSystem::COMPRESSION_NAME);
	}
};

} // namespace

unique_ptr<FileHandle> SnappyFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                            bool write) {
	auto path = handle->path;
	return make_uniq<SnappyFile>(context, std::move(handle), path, write);
}

bool SnappyFileSystem::CanHandleFile(const string &fpath) {
	return CompressionPathUtils::HasExtension(fpath, {".sz", ".snappy"});
}

unique_ptr<StreamWrapper> SnappyFileSystem::CreateStream() {
	return make_uniq<SnappyStreamWrapper>();
}

idx_t SnappyFileSystem::InBufferSize() {
	return idx_t(1) << 17;
}

idx_t SnappyFileSystem::OutBufferSize() {
	return SnappyFrame::MAX_UNCOMPRESSED_CHUNK_SIZE;
}

} // namespace duckdb
