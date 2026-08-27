#include "crc32c.hpp"

namespace duckdb {

uint32_t CRC32C::Compute(const void *data, idx_t size) {
	static constexpr uint32_t POLYNOMIAL = 0x82F63B78;
	auto crc = uint32_t(0xFFFFFFFF);
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (idx_t byte_idx = 0; byte_idx < size; byte_idx++) {
		crc ^= bytes[byte_idx];
		for (idx_t bit_idx = 0; bit_idx < 8; bit_idx++) {
			const auto mask = uint32_t(0) - (crc & 1);
			crc = (crc >> 1) ^ (POLYNOMIAL & mask);
		}
	}
	return ~crc;
}

uint32_t CRC32C::ComputeMasked(const void *data, idx_t size) {
	const auto crc = Compute(data, size);
	return ((crc >> 15) | (crc << 17)) + 0xA282EAD8;
}

} // namespace duckdb
