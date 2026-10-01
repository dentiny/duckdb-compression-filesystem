#include "lzo_file_system.hpp"

#include "compression_path_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "lzokay.hpp"
#include "miniz.hpp"

#include <cstring>

namespace duckdb {
namespace {

constexpr data_t MAGIC[] = {0x89, 'L', 'Z', 'O', 0, 13, 10, 26, 10};
constexpr uint32_t ADLER_D = 0x0001, ADLER_C = 0x0002, CRC_D = 0x0100, CRC_C = 0x0200;
constexpr uint32_t HEADER_CRC = 0x1000;
constexpr idx_t BLOCK_SIZE = 256 * 1024;
constexpr idx_t MAX_BLOCK_SIZE = 64 * 1024 * 1024;

uint32_t LoadBE(const_data_ptr_t p, idx_t size = 4) {
	uint32_t result = 0;
	for (idx_t i = 0; i < size; i++) {
		result = (result << 8) | p[i];
	}
	return result;
}

void AppendBE(vector<data_t> &buffer, uint32_t value, idx_t size = 4) {
	for (idx_t i = size; i > 0; i--) {
		buffer.push_back(data_t(value >> ((i - 1) * 8)));
	}
}

uint32_t Checksum(const_data_ptr_t data, idx_t size, bool crc) {
	return NumericCast<uint32_t>(crc ? duckdb_miniz::mz_crc32(0, data, size) : duckdb_miniz::mz_adler32(1, data, size));
}

struct LzoFileSystemHolder {
	LzoFileSystem fs;
};

class LzoFile : private LzoFileSystemHolder, public CompressedFile {
public:
	LzoFile(QueryContext context, unique_ptr<FileHandle> handle, const string &path, bool write)
	    : CompressedFile(fs, std::move(handle), path) {
		Initialize(context, write);
	}

	bool read_finished = false;
	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(LzoFileSystem::COMPRESSION_NAME);
	}
};

struct LzoStreamWrapper : public StreamWrapper {
	void Initialize(QueryContext context, CompressedFile &file, bool write) override;
	bool Read(StreamData &sd) override;
	void Write(CompressedFile &file, StreamData &sd, data_ptr_t buffer, int64_t size) override;
	void Close() override;
	void AbortWrite() override;

private:
	enum class Phase { HEADER, SIZE, BLOCK_HEADER, BLOCK };
	bool Collect(StreamData &sd, idx_t size);
	void FlushBlock();
	void WriteBytes(const_data_ptr_t data, idx_t size);

	CompressedFile *file = nullptr;
	bool writing = false;
	Phase phase = Phase::HEADER;
	vector<data_t> record;
	vector<data_t> pending;
	unique_ptr<lzokay::Dict<>> dictionary;
	uint32_t flags = 0;
	uint32_t decoded_size = 0, encoded_size = 0;
	uint32_t checksums[4] = {};
};

void LzoStreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	file = &file_p;
	file->Cast<LzoFile>().read_finished = false;
	writing = write;
	if (!write) {
		return;
	}
	dictionary = make_uniq<lzokay::Dict<>>();
	pending.reserve(BLOCK_SIZE);
	vector<data_t> header(MAGIC, MAGIC + sizeof(MAGIC));
	AppendBE(header, 0x1040, 2); // lzop format version
	AppendBE(header, 0x2060, 2); // LZO1X-compatible library format
	AppendBE(header, 0x0940, 2); // minimum reader version
	header.push_back(3);         // LZO1X-999-compatible compression
	header.push_back(9);
	AppendBE(header, ADLER_D | ADLER_C);
	AppendBE(header, 0); // mode
	AppendBE(header, 0); // mtime low
	AppendBE(header, 0); // mtime high
	header.push_back(0); // no original filename
	AppendBE(header, Checksum(header.data() + sizeof(MAGIC), header.size() - sizeof(MAGIC), false));
	WriteBytes(header.data(), header.size());
}

// Accumulate one bounded header/block across CompressedFile input buffers.
bool LzoStreamWrapper::Collect(StreamData &sd, idx_t size) {
	if (record.size() >= size) {
		return true;
	}
	const auto count = MinValue<idx_t>(size - record.size(), sd.in_buff_end - sd.in_buff_start);
	const auto offset = record.size();
	record.resize(offset + count);
	if (count) {
		memcpy(record.data() + offset, sd.in_buff_start, count);
		sd.in_buff_start += count;
	}
	return record.size() == size;
}

bool LzoStreamWrapper::Read(StreamData &sd) {
	const uint32_t checksum_flags[] = {ADLER_D, CRC_D, ADLER_C, CRC_C};
	while (true) {
		switch (phase) {
		case Phase::HEADER: {
			if (record.empty() && sd.in_buff_start == sd.in_buff_end) {
				return false;
			}
			file->Cast<LzoFile>().read_finished = false;
			if (!Collect(sd, 15)) {
				return false;
			}
			if (memcmp(record.data(), MAGIC, sizeof(MAGIC)) != 0) {
				throw IOException("Invalid lzop magic; expected an lzop container");
			}
			const auto version = LoadBE(record.data() + 9, 2);
			if (version < 0x0900) {
				throw IOException("Unsupported lzop version");
			}
			const bool modern = version >= 0x0940;
			const idx_t fixed_size = modern ? 34 : 27;
			if (!Collect(sd, fixed_size)) {
				return false;
			}
			const auto method = record[modern ? 15 : 13];
			if (method < 1 || method > 3 || (modern && (LoadBE(record.data() + 13, 2) > 0x1040 || record[16] > 9))) {
				throw IOException("Unsupported lzop compression method or version");
			}
			flags = LoadBE(record.data() + (modern ? 17 : 14));
			// Reject extra fields, multipart streams, filters, and reserved bits.
			// OS/charset and filename metadata flags do not affect decoding.
			if (flags & (0x0040 | 0x0400 | 0x0800 | 0x000fc000)) {
				throw IOException("Unsupported lzop header flags");
			}
			const auto header_size = fixed_size + record[fixed_size - 1];
			if (!Collect(sd, header_size + 4)) {
				return false;
			}
			if (LoadBE(record.data() + header_size) !=
			    Checksum(record.data() + sizeof(MAGIC), header_size - sizeof(MAGIC), flags & HEADER_CRC)) {
				throw IOException("LZO header checksum mismatch");
			}
			record.clear();
			phase = Phase::SIZE;
			break;
		}
		case Phase::SIZE:
			if (!Collect(sd, 4)) {
				return false;
			}
			decoded_size = LoadBE(record.data());
			if (decoded_size == 0) {
				file->Cast<LzoFile>().read_finished = true;
				record.clear();
				phase = Phase::HEADER; // Allow concatenated lzop members.
				break;
			}
			if (decoded_size > MAX_BLOCK_SIZE) {
				throw IOException("LZO block exceeds maximum size (64 MiB)");
			}
			phase = Phase::BLOCK_HEADER;
			break;
		case Phase::BLOCK_HEADER: {
			if (!Collect(sd, 8)) {
				return false;
			}
			encoded_size = LoadBE(record.data() + 4);
			if (encoded_size == 0 || encoded_size > decoded_size) {
				throw IOException("Invalid LZO block size");
			}
			idx_t header_size = 8;
			for (idx_t i = 0; i < 4; i++) {
				if ((flags & checksum_flags[i]) && (i < 2 || encoded_size < decoded_size)) {
					header_size += 4;
				}
			}
			if (!Collect(sd, header_size)) {
				return false;
			}
			idx_t offset = 8;
			for (idx_t i = 0; i < 4; i++) {
				if ((flags & checksum_flags[i]) && (i < 2 || encoded_size < decoded_size)) {
					checksums[i] = LoadBE(record.data() + offset);
					offset += 4;
				}
			}
			record.clear();
			phase = Phase::BLOCK;
			break;
		}
		case Phase::BLOCK: {
			if (!Collect(sd, encoded_size)) {
				return false;
			}
			for (idx_t i = 2; i < 4; i++) {
				if (encoded_size < decoded_size && (flags & checksum_flags[i]) &&
				    checksums[i] != Checksum(record.data(), encoded_size, i == 3)) {
					throw IOException("LZO compressed block checksum mismatch");
				}
			}
			if (decoded_size > sd.out_buf_size) {
				sd.out_buff = make_unsafe_uniq_array<data_t>(decoded_size);
				sd.out_buf_size = decoded_size;
			}
			if (encoded_size == decoded_size) {
				memcpy(sd.out_buff.get(), record.data(), decoded_size);
			} else {
				size_t size = 0;
				const auto result =
				    lzokay::decompress(record.data(), encoded_size, sd.out_buff.get(), decoded_size, size);
				if (result != lzokay::EResult::Success || size != decoded_size) {
					throw IOException("Invalid LZO compressed block");
				}
			}
			for (idx_t i = 0; i < 2; i++) {
				if ((flags & checksum_flags[i]) && checksums[i] != Checksum(sd.out_buff.get(), decoded_size, i == 1)) {
					throw IOException("LZO block checksum mismatch");
				}
			}
			sd.out_buff_start = sd.out_buff.get();
			sd.out_buff_end = sd.out_buff_start + decoded_size;
			record.clear();
			phase = Phase::SIZE;
			return false;
		}
		}
	}
}

void LzoStreamWrapper::WriteBytes(const_data_ptr_t data, idx_t size) {
	file->child_handle->Write(file->context, const_cast<data_ptr_t>(data), size);
}

void LzoStreamWrapper::FlushBlock() {
	if (pending.empty()) {
		return;
	}
	vector<data_t> compressed(lzokay::compress_worst_size(pending.size()));
	size_t size = 0;
	if (lzokay::compress(pending.data(), pending.size(), compressed.data(), compressed.size(), size, *dictionary) !=
	    lzokay::EResult::Success) {
		throw IOException("Failed to compress LZO block");
	}
	const bool use_compressed = size < pending.size();
	vector<data_t> header;
	AppendBE(header, NumericCast<uint32_t>(pending.size()));
	AppendBE(header, NumericCast<uint32_t>(use_compressed ? size : pending.size()));
	AppendBE(header, Checksum(pending.data(), pending.size(), false));
	if (use_compressed) {
		AppendBE(header, Checksum(compressed.data(), size, false));
	}
	WriteBytes(header.data(), header.size());
	WriteBytes(use_compressed ? compressed.data() : pending.data(), use_compressed ? size : pending.size());
	pending.clear();
}

void LzoStreamWrapper::Write(CompressedFile &, StreamData &, data_ptr_t buffer, int64_t size) {
	auto remaining = NumericCast<idx_t>(size);
	while (remaining) {
		const auto count = MinValue<idx_t>(remaining, BLOCK_SIZE - pending.size());
		pending.insert(pending.end(), buffer, buffer + count);
		buffer += count;
		remaining -= count;
		if (pending.size() == BLOCK_SIZE) {
			FlushBlock();
		}
	}
}

void LzoStreamWrapper::Close() {
	if (file && writing) {
		FlushBlock();
		const data_t end[] = {0, 0, 0, 0};
		WriteBytes(end, sizeof(end));
	}
	AbortWrite();
}

void LzoStreamWrapper::AbortWrite() {
	file = nullptr;
	writing = false;
	dictionary.reset();
	pending.clear();
	record.clear();
}

} // namespace

unique_ptr<FileHandle> LzoFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                         bool write) {
	auto path = handle->path;
	return make_uniq<LzoFile>(context, std::move(handle), path, write);
}

int64_t LzoFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	const auto result = CompressedFileSystem::Read(handle, buffer, nr_bytes);
	if (result < nr_bytes && !handle.Cast<LzoFile>().read_finished) {
		throw IOException("Truncated LZO stream");
	}
	return result;
}

bool LzoFileSystem::CanHandleFile(const string &path) {
	return CompressionPathUtils::HasExtension(path, ".lzo");
}

unique_ptr<StreamWrapper> LzoFileSystem::CreateStream() {
	return make_uniq<LzoStreamWrapper>();
}

idx_t LzoFileSystem::InBufferSize() {
	return idx_t(1) << 16;
}

idx_t LzoFileSystem::OutBufferSize() {
	return BLOCK_SIZE;
}

} // namespace duckdb
