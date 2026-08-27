#pragma once

#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

class BrotliFileSystem : public CompressedFileSystem {
public:
	static constexpr const char *COMPRESSION_NAME = "brotli";

	unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle, bool write) override;

	string GetName() const override {
		return "BrotliFileSystem";
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
