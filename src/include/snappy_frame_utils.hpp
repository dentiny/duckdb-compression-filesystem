#pragma once

#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

class SnappyFrame {
public:
	static constexpr uint8_t COMPRESSED_CHUNK = 0x00;
	static constexpr uint8_t UNCOMPRESSED_CHUNK = 0x01;
	static constexpr uint8_t STREAM_IDENTIFIER_CHUNK = 0xFF;
	static constexpr uint8_t PADDING_CHUNK = 0xFE;
	static constexpr uint8_t FIRST_SKIPPABLE_CHUNK = 0x80;
	static constexpr uint8_t LAST_UNSKIPPABLE_CHUNK = 0x7F;

	static constexpr idx_t CHUNK_HEADER_SIZE = 4;
	static constexpr idx_t CHECKSUM_SIZE = 4;
	static constexpr idx_t MAX_UNCOMPRESSED_CHUNK_SIZE = idx_t(1) << 16;

	static uint32_t LoadLittleEndian32(const_data_ptr_t source);
	static void StoreLittleEndian32(data_ptr_t destination, uint32_t value);
	static uint32_t LoadLittleEndian24(const_data_ptr_t source);
	static void StoreLittleEndian24(data_ptr_t destination, uint32_t value);
	static bool IsSkippableChunk(uint8_t chunk_type);
	static bool IsStreamIdentifier(const_data_ptr_t payload, idx_t payload_size);
};

class SnappyInputCursor {
public:
	explicit SnappyInputCursor(StreamData &stream_data) : stream_data(stream_data) {
	}

	idx_t Available() const;
	bool NeedsMore(idx_t count, const char *description);
	void Consume(idx_t count);
	const_data_ptr_t Current() const;

private:
	StreamData &stream_data;
};

} // namespace duckdb
