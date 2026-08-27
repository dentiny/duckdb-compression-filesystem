#pragma once

#include "duckdb/common/string.hpp"

#include <initializer_list>

namespace duckdb {

class CompressionPathUtils {
public:
	static bool HasExtension(const string &path, const char *extension);
	static bool HasExtension(const string &path, std::initializer_list<const char *> extensions);

private:
	static string NormalizeForExtensionCheck(const string &path);
};

} // namespace duckdb
