#pragma once

#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

class XzFileSystem : public CompressedFileSystem {
public:
	static constexpr const char *COMPRESSION_NAME = "xz";
	enum class Container { XZ, LZMA_ALONE };

	explicit XzFileSystem(Container container = Container::XZ) : container(container) {
	}

	unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle, bool write) override;

	string GetName() const override {
		return container == Container::XZ ? "XzFileSystem" : "LzmaFileSystem";
	}

	FileCompressionType GetCompressionType() override {
		return FileCompressionType(container == Container::XZ ? COMPRESSION_NAME : "lzma");
	}

	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	bool CanHandleFile(const string &fpath) override;

	unique_ptr<StreamWrapper> CreateStream() override;
	idx_t InBufferSize() override;
	idx_t OutBufferSize() override;

private:
	Container container;
};

class LzmaFileSystem : public XzFileSystem {
public:
	static constexpr const char *COMPRESSION_NAME = "lzma";

	LzmaFileSystem() : XzFileSystem(Container::LZMA_ALONE) {
	}
};

} // namespace duckdb
