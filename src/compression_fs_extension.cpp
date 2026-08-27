#define DUCKDB_EXTENSION_MAIN

#include "brotli_file_system.hpp"
#include "bzip2_file_system.hpp"
#include "compression_fs_extension.hpp"
#include "lz4_file_system.hpp"
#include "snappy_file_system.hpp"
#include "xz_file_system.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription(
	    "Compression filesystems that DuckDB does not ship natively (brotli, bzip2, lz4, snappy, xz)");
	auto &fs = loader.GetDatabaseInstance().GetFileSystem();
	fs.RegisterCompressionFilesystem(make_uniq<BrotliFileSystem>());
	fs.RegisterCompressionFilesystem(make_uniq<Bzip2FileSystem>());
	fs.RegisterCompressionFilesystem(make_uniq<Lz4FileSystem>());
	fs.RegisterCompressionFilesystem(make_uniq<SnappyFileSystem>());
	fs.RegisterCompressionFilesystem(make_uniq<XzFileSystem>());
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
