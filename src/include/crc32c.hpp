#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class CRC32C {
public:
	static uint32_t Compute(const void *data, idx_t size);
	static uint32_t ComputeMasked(const void *data, idx_t size);
};

} // namespace duckdb
