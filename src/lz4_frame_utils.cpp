#include "lz4_frame_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cstring>

namespace duckdb {

uint32_t Lz4Frame::LoadLittleEndian32(const_data_ptr_t source) {
	return uint32_t(source[0]) | (uint32_t(source[1]) << 8) | (uint32_t(source[2]) << 16) | (uint32_t(source[3]) << 24);
}

void Lz4Frame::StoreLittleEndian32(data_ptr_t destination, uint32_t value) {
	destination[0] = data_t(value);
	destination[1] = data_t(value >> 8);
	destination[2] = data_t(value >> 16);
	destination[3] = data_t(value >> 24);
}

bool Lz4Frame::IsSkippableMagic(uint32_t magic) {
	return magic >= SKIPPABLE_MAGIC_MIN && magic <= SKIPPABLE_MAGIC_MAX;
}

idx_t Lz4Frame::DecodeMaximumBlockSize(uint8_t block_descriptor) {
	if (block_descriptor & BD_RESERVED_MASK) {
		throw IOException("Invalid LZ4 frame block descriptor");
	}
	const auto code = (block_descriptor >> BD_BLOCK_SIZE_SHIFT) & 0x07;
	switch (code) {
	case 4:
		return idx_t(1) << 16;
	case 5:
		return idx_t(1) << 18;
	case 6:
		return idx_t(1) << 20;
	case 7:
		return MAX_BLOCK_SIZE;
	default:
		throw IOException("Unsupported LZ4 frame max block size code %u", code);
	}
}

idx_t Lz4InputCursor::Available() const {
	return UnsafeNumericCast<idx_t>(stream_data.in_buff_end - stream_data.in_buff_start);
}

bool Lz4InputCursor::NeedsMore(idx_t count, const char *description) {
	if (Available() >= count) {
		return false;
	}
	if (stream_data.in_buff_end == stream_data.in_buff.get() + stream_data.in_buf_size) {
		stream_data.refresh = true;
		return true;
	}
	throw IOException("Truncated LZ4 frame while reading %s", description);
}

void Lz4InputCursor::Consume(idx_t count) {
	stream_data.in_buff_start += count;
}

const_data_ptr_t Lz4InputCursor::Current() const {
	return stream_data.in_buff_start;
}

XXH32Hasher::XXH32Hasher() {
	Reset();
}

void XXH32Hasher::Reset(uint32_t seed_p) {
	seed = seed_p;
	total_length = 0;
	buffer_size = 0;
	has_stripe = false;
	lane_1 = seed + PRIME_1 + PRIME_2;
	lane_2 = seed + PRIME_2;
	lane_3 = seed;
	lane_4 = seed - PRIME_1;
}

void XXH32Hasher::Update(const void *input, size_t length) {
	const auto *cursor = static_cast<const uint8_t *>(input);
	total_length += length;
	if (buffer_size + length < sizeof(buffer)) {
		memcpy(buffer + buffer_size, cursor, length);
		buffer_size += uint32_t(length);
		return;
	}
	if (buffer_size) {
		const auto fill = sizeof(buffer) - buffer_size;
		memcpy(buffer + buffer_size, cursor, fill);
		ProcessStripe(buffer);
		cursor += fill;
		length -= fill;
		buffer_size = 0;
		has_stripe = true;
	}
	while (length >= sizeof(buffer)) {
		ProcessStripe(cursor);
		cursor += sizeof(buffer);
		length -= sizeof(buffer);
		has_stripe = true;
	}
	if (length) {
		memcpy(buffer, cursor, length);
		buffer_size = uint32_t(length);
	}
}

uint32_t XXH32Hasher::Digest() const {
	uint32_t hash;
	if (has_stripe) {
		hash = RotateLeft(lane_1, 1) + RotateLeft(lane_2, 7) + RotateLeft(lane_3, 12) + RotateLeft(lane_4, 18);
	} else {
		hash = seed + PRIME_5;
	}
	hash += uint32_t(total_length);

	const auto *cursor = buffer;
	const auto *end = buffer + buffer_size;
	while (cursor + sizeof(uint32_t) <= end) {
		hash += Lz4Frame::LoadLittleEndian32(cursor) * PRIME_3;
		hash = RotateLeft(hash, 17) * PRIME_4;
		cursor += sizeof(uint32_t);
	}
	while (cursor < end) {
		hash += (*cursor) * PRIME_5;
		hash = RotateLeft(hash, 11) * PRIME_1;
		cursor++;
	}
	hash ^= hash >> 15;
	hash *= PRIME_2;
	hash ^= hash >> 13;
	hash *= PRIME_3;
	hash ^= hash >> 16;
	return hash;
}

uint32_t XXH32Hasher::Hash(const void *input, size_t length, uint32_t seed) {
	XXH32Hasher hasher;
	hasher.Reset(seed);
	hasher.Update(input, length);
	return hasher.Digest();
}

uint32_t XXH32Hasher::RotateLeft(uint32_t value, int count) {
	return (value << count) | (value >> (32 - count));
}

void XXH32Hasher::ProcessStripe(const uint8_t *input) {
	lane_1 = RotateLeft(lane_1 + Lz4Frame::LoadLittleEndian32(input) * PRIME_2, 13) * PRIME_1;
	lane_2 = RotateLeft(lane_2 + Lz4Frame::LoadLittleEndian32(input + 4) * PRIME_2, 13) * PRIME_1;
	lane_3 = RotateLeft(lane_3 + Lz4Frame::LoadLittleEndian32(input + 8) * PRIME_2, 13) * PRIME_1;
	lane_4 = RotateLeft(lane_4 + Lz4Frame::LoadLittleEndian32(input + 12) * PRIME_2, 13) * PRIME_1;
}

} // namespace duckdb
