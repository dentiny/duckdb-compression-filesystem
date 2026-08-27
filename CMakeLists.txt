cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME compression_fs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED DUCKDB_MODULE_BASE_DIR)
  set(DUCKDB_BROTLI_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/brotli")
  set(DUCKDB_LZ4_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/lz4")
  set(DUCKDB_SNAPPY_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/snappy")
else()
  set(DUCKDB_BROTLI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/brotli")
  set(DUCKDB_LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/lz4")
  set(DUCKDB_SNAPPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/snappy")
endif()

include_directories(
  src/include
  "${DUCKDB_BROTLI_DIR}/include"
  ${DUCKDB_LZ4_DIR}
  ${DUCKDB_SNAPPY_DIR})

set(EXTENSION_SOURCES
    src/brotli_file_system.cpp
    src/compression_fs_extension.cpp
    src/crc32c.cpp
    src/lz4_frame_utils.cpp
    src/lz4_file_system.cpp
    src/snappy_frame_utils.cpp
    src/snappy_file_system.cpp)

# In-tree codecs. The parquet static library already compiles these sources, so
# only add them to our static target when parquet is not linked into the binary.
set(LZ4_SOURCE "${DUCKDB_LZ4_DIR}/lz4.cpp")
set(SNAPPY_SOURCES
    "${DUCKDB_SNAPPY_DIR}/snappy.cc"
    "${DUCKDB_SNAPPY_DIR}/snappy-sinksource.cc")
set(BROTLI_SOURCES
    "${DUCKDB_BROTLI_DIR}/enc/dictionary_hash.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/backward_references_hq.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/histogram.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/memory.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/entropy_encode.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/compound_dictionary.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/compress_fragment_two_pass.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/block_splitter.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/command.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/encode.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/encoder_dict.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/cluster.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/backward_references.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/utf8_util.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/compress_fragment.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/fast_log.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/brotli_bit_stream.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/bit_cost.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/static_dict.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/literal_cost.cpp"
    "${DUCKDB_BROTLI_DIR}/enc/metablock.cpp"
    "${DUCKDB_BROTLI_DIR}/common/dictionary.cpp"
    "${DUCKDB_BROTLI_DIR}/common/constants.cpp"
    "${DUCKDB_BROTLI_DIR}/common/transform.cpp"
    "${DUCKDB_BROTLI_DIR}/common/platform.cpp"
    "${DUCKDB_BROTLI_DIR}/common/shared_dictionary.cpp"
    "${DUCKDB_BROTLI_DIR}/common/context.cpp"
    "${DUCKDB_BROTLI_DIR}/dec/state.cpp"
    "${DUCKDB_BROTLI_DIR}/dec/decode.cpp"
    "${DUCKDB_BROTLI_DIR}/dec/huffman.cpp"
    "${DUCKDB_BROTLI_DIR}/dec/bit_reader.cpp")
set(STATIC_SOURCES ${EXTENSION_SOURCES})
if(NOT DUCKDB_EXTENSION_PARQUET_SHOULD_LINK)
  list(APPEND STATIC_SOURCES ${BROTLI_SOURCES} ${LZ4_SOURCE} ${SNAPPY_SOURCES})
endif()

build_static_extension(${TARGET_NAME} ${STATIC_SOURCES})
build_loadable_extension(
  ${TARGET_NAME}
  " "
  ${EXTENSION_SOURCES}
  ${BROTLI_SOURCES}
  ${LZ4_SOURCE}
  ${SNAPPY_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
