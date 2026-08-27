#include "compression_path_utils.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

bool CompressionPathUtils::HasExtension(const string &path, const char *extension) {
	return HasExtension(path, {extension});
}

bool CompressionPathUtils::HasExtension(const string &path, std::initializer_list<const char *> extensions) {
	const auto normalized_path = NormalizeForExtensionCheck(path);
	for (const auto extension : extensions) {
		if (StringUtil::EndsWith(normalized_path, extension)) {
			return true;
		}
	}
	return false;
}

string CompressionPathUtils::NormalizeForExtensionCheck(const string &path) {
	auto normalized_path = path;
	if (!StringUtil::StartsWith(normalized_path, "\\\\?\\")) {
		const auto question_mark_pos = normalized_path.find('?');
		normalized_path = normalized_path.substr(0, question_mark_pos);
	}
	return StringUtil::Lower(normalized_path);
}

} // namespace duckdb
