#include "snappy_frame_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cstring>

namespace duckdb {

uint32_t SnappyFrame::LoadLittleEndian32(const_data_ptr_t source) {
	return uint32_t(source[0]) | (uint32_t(source[1]) << 8) | (uint32_t(source[2]) << 16) | (uint32_t(source[3]) << 24);
}

void SnappyFrame::StoreLittleEndian32(data_ptr_t destination, uint32_t value) {
	destination[0] = data_t(value);
	destination[1] = data_t(value >> 8);
	destination[2] = data_t(value >> 16);
	destination[3] = data_t(value >> 24);
}

uint32_t SnappyFrame::LoadLittleEndian24(const_data_ptr_t source) {
	return uint32_t(source[0]) | (uint32_t(source[1]) << 8) | (uint32_t(source[2]) << 16);
}

void SnappyFrame::StoreLittleEndian24(data_ptr_t destination, uint32_t value) {
	destination[0] = data_t(value);
	destination[1] = data_t(value >> 8);
	destination[2] = data_t(value >> 16);
}

bool SnappyFrame::IsSkippableChunk(uint8_t chunk_type) {
	return chunk_type >= FIRST_SKIPPABLE_CHUNK && chunk_type != STREAM_IDENTIFIER_CHUNK;
}

bool SnappyFrame::IsStreamIdentifier(const_data_ptr_t payload, idx_t payload_size) {
	static constexpr data_t IDENTIFIER[] = {'s', 'N', 'a', 'P', 'p', 'Y'};
	return payload_size == sizeof(IDENTIFIER) && memcmp(payload, IDENTIFIER, sizeof(IDENTIFIER)) == 0;
}

idx_t SnappyInputCursor::Available() const {
	return UnsafeNumericCast<idx_t>(stream_data.in_buff_end - stream_data.in_buff_start);
}

bool SnappyInputCursor::NeedsMore(idx_t count, const char *description) {
	if (Available() >= count) {
		return false;
	}
	if (stream_data.in_buff_end == stream_data.in_buff.get() + stream_data.in_buf_size) {
		stream_data.refresh = true;
		return true;
	}
	throw IOException("Truncated Snappy framed stream while reading %s", description);
}

void SnappyInputCursor::Consume(idx_t count) {
	stream_data.in_buff_start += count;
}

const_data_ptr_t SnappyInputCursor::Current() const {
	return stream_data.in_buff_start;
}

} // namespace duckdb
