#pragma once

#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

class Bzip2FileSystem : public CompressedFileSystem {
public:
	static constexpr const char *COMPRESSION_NAME = "bzip2";

	unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle, bool write) override;

	string GetName() const override {
		return "Bzip2FileSystem";
	}

	FileCompressionType GetCompressionType() override {
		return FileCompressionType(COMPRESSION_NAME);
	}

	bool CanHandleFile(const string &fpath) override;

	unique_ptr<StreamWrapper> CreateStream() override;
	idx_t InBufferSize() override;
	idx_t OutBufferSize() override;
};

} // namespace duckdb
