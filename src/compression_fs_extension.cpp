#define DUCKDB_EXTENSION_MAIN

#include "compression_fs_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void CompressionFsScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void CompressionFsOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "CompressionFs " + name.GetString() +
		                                           ", my linked OpenSSL version is " + OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto compression_fs_scalar_function =
	    ScalarFunction("compression_fs", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CompressionFsScalarFun);

	loader.RegisterFunction(compression_fs_scalar_function);

	// Register another scalar function
	auto compression_fs_openssl_version_scalar_function =
	    ScalarFunction("compression_fs_openssl_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                   CompressionFsOpenSSLVersionScalarFun);
	loader.RegisterFunction(compression_fs_openssl_version_scalar_function);
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
