#define DUCKDB_EXTENSION_MAIN

#include "compression_fs_extension.hpp"
#include "lz4_file_system.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("Compression filesystems that DuckDB does not ship natively (lz4)");
	auto &fs = loader.GetDatabaseInstance().GetFileSystem();
	fs.RegisterCompressionFilesystem(make_uniq<Lz4FileSystem>());
}

void CompressionFsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string CompressionFsExtension::Name() {
	return "compression_fs";
}

std::string CompressionFsExtension::Version() const {
#ifdef EXT_VERSION_COMPRESSION_FS
	return EXT_VERSION_COMPRESSION_FS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(compression_fs, loader) {
	duckdb::LoadInternal(loader);
}
}
