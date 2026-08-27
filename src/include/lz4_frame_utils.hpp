#pragma once

#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

class Lz4Frame {
public:
	static constexpr uint32_t MAGIC = 0x184D2204;
	static constexpr uint32_t SKIPPABLE_MAGIC_MIN = 0x184D2A50;
	static constexpr uint32_t SKIPPABLE_MAGIC_MAX = 0x184D2A5F;
	static constexpr uint32_t LEGACY_MAGIC = 0x184C2102;

	static constexpr uint8_t FLG_VERSION_MASK = 0xC0;
	static constexpr uint8_t FLG_VERSION = 0x40;
	static constexpr uint8_t FLG_BLOCK_INDEPENDENT = 0x20;
	static constexpr uint8_t FLG_BLOCK_CHECKSUM = 0x10;
	static constexpr uint8_t FLG_CONTENT_SIZE = 0x08;
	static constexpr uint8_t FLG_CONTENT_CHECKSUM = 0x04;
	static constexpr uint8_t FLG_RESERVED = 0x02;
	static constexpr uint8_t FLG_DICTIONARY_ID = 0x01;

	static constexpr uint8_t BD_BLOCK_SIZE_SHIFT = 4;
	static constexpr uint8_t BD_RESERVED_MASK = 0x8F;

	static constexpr idx_t MAX_BLOCK_SIZE = idx_t(1) << 22;
	static constexpr idx_t WRITE_BLOCK_SIZE = idx_t(1) << 16;
	static constexpr idx_t HISTORY_SIZE = 65536;
	static constexpr uint32_t UNCOMPRESSED_BIT = 0x80000000;

	static constexpr uint8_t WRITE_FLG = FLG_VERSION | FLG_BLOCK_INDEPENDENT | FLG_CONTENT_CHECKSUM;
	static constexpr uint8_t WRITE_BD = 4 << BD_BLOCK_SIZE_SHIFT;

	static uint32_t LoadLittleEndian32(const_data_ptr_t source);
	static void StoreLittleEndian32(data_ptr_t destination, uint32_t value);
	static bool IsSkippableMagic(uint32_t magic);
	static idx_t DecodeMaximumBlockSize(uint8_t block_descriptor);
};

class Lz4InputCursor {
public:
	explicit Lz4InputCursor(StreamData &stream_data) : stream_data(stream_data) {
	}

	idx_t Available() const;
	bool NeedsMore(idx_t count, const char *description);
	void Consume(idx_t count);
	const_data_ptr_t Current() const;

private:
	StreamData &stream_data;
};

class XXH32Hasher {
public:
	XXH32Hasher();

	void Reset(uint32_t seed = 0);
	void Update(const void *input, size_t length);
	uint32_t Digest() const;

	static uint32_t Hash(const void *input, size_t length, uint32_t seed = 0);

private:
	static constexpr uint32_t PRIME_1 = 2654435761u;
	static constexpr uint32_t PRIME_2 = 2246822519u;
	static constexpr uint32_t PRIME_3 = 3266489917u;
	static constexpr uint32_t PRIME_4 = 668265263u;
	static constexpr uint32_t PRIME_5 = 374761393u;

	static uint32_t RotateLeft(uint32_t value, int count);
	void ProcessStripe(const uint8_t *input);

	uint32_t seed = 0;
	uint32_t lane_1 = 0;
	uint32_t lane_2 = 0;
	uint32_t lane_3 = 0;
	uint32_t lane_4 = 0;
	uint64_t total_length = 0;
	uint8_t buffer[16] = {};
	uint32_t buffer_size = 0;
	bool has_stripe = false;
};

} // namespace duckdb
