cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME compression_fs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(BROTLI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/brotli")
set(BZIP2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bzip2")
set(LZOKAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lzokay")
set(XZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xz")

# Build only the static liblzma target from the XZ submodule.
set(XZ_NLS OFF)
set(XZ_THREADS no)
set(XZ_SANDBOX no)
set(XZ_TOOL_XZ OFF)
set(XZ_TOOL_XZDEC OFF)
set(XZ_TOOL_LZMADEC OFF)
set(XZ_TOOL_LZMAINFO OFF)
set(XZ_DOC OFF)
set(BUILD_SHARED_LIBS_WAS_DEFINED FALSE)
if(DEFINED BUILD_SHARED_LIBS)
  set(BUILD_SHARED_LIBS_WAS_DEFINED TRUE)
  set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
endif()
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${XZ_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/xz" EXCLUDE_FROM_ALL)
# Keep upstream Brotli's options scoped to its build. DuckDB uses a boolean
# ENABLE_SANITIZER, whereas Brotli expects a sanitizer name in that variable.
function(compression_fs_add_brotli)
  set(ENABLE_SANITIZER "")
  set(BROTLI_BUILD_TOOLS
      OFF
      CACHE BOOL "Build Brotli CLI tools")
  set(BROTLI_DISABLE_TESTS ON)
  set(BROTLI_BUNDLED_MODE ON)
  add_subdirectory("${BROTLI_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/brotli"
                   EXCLUDE_FROM_ALL)
endfunction()
compression_fs_add_brotli()
# Keep the pinned codec private when loaded alongside other Brotli users.
set_target_properties(brotlicommon brotlidec brotlienc
                      PROPERTIES C_VISIBILITY_PRESET hidden)
if(BUILD_SHARED_LIBS_WAS_DEFINED)
  set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
else()
  unset(BUILD_SHARED_LIBS)
endif()

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED DUCKDB_MODULE_BASE_DIR)
  set(DUCKDB_MINIZ_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/miniz")
  set(DUCKDB_LZ4_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/lz4")
  set(DUCKDB_SNAPPY_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/snappy")
else()
  set(DUCKDB_MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/miniz")
  set(DUCKDB_LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/lz4")
  set(DUCKDB_SNAPPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/snappy")
endif()

include_directories(
  src/include
  ${BZIP2_DIR}
  ${LZOKAY_DIR}
  ${DUCKDB_LZ4_DIR}
  ${DUCKDB_SNAPPY_DIR}
  ${DUCKDB_MINIZ_DIR}
  "${XZ_DIR}/src/liblzma/api")

set(EXTENSION_SOURCES
    src/brotli_file_system.cpp
    src/bzip2_file_system.cpp
    src/compression_fs_extension.cpp
    src/compression_path_utils.cpp
    src/crc32c.cpp
    src/deflate_file_system.cpp
    src/lz4_frame_utils.cpp
    src/lz4_file_system.cpp
    src/lzo_file_system.cpp
    src/snappy_frame_utils.cpp
    src/snappy_file_system.cpp
    src/xz_file_system.cpp)

# In-tree codecs. The parquet static library already compiles these sources, so
# only add them to our static target when parquet is not linked into the binary.
set(LZ4_SOURCE "${DUCKDB_LZ4_DIR}/lz4.cpp")
set(SNAPPY_SOURCES "${DUCKDB_SNAPPY_DIR}/snappy.cc"
                   "${DUCKDB_SNAPPY_DIR}/snappy-sinksource.cc")
set(BZIP2_SOURCES
    "${BZIP2_DIR}/blocksort.c"
    "${BZIP2_DIR}/huffman.c"
    "${BZIP2_DIR}/crctable.c"
    "${BZIP2_DIR}/randtable.c"
    "${BZIP2_DIR}/compress.c"
    "${BZIP2_DIR}/decompress.c"
    "${BZIP2_DIR}/bzlib.c")
set(LZOKAY_SOURCE "${LZOKAY_DIR}/lzokay.cpp")
set(STATIC_SOURCES ${EXTENSION_SOURCES} ${BZIP2_SOURCES} ${LZOKAY_SOURCE})
if(NOT DUCKDB_EXTENSION_PARQUET_SHOULD_LINK)
  list(APPEND STATIC_SOURCES ${LZ4_SOURCE} ${SNAPPY_SOURCES})
endif()

build_static_extension(${TARGET_NAME} ${STATIC_SOURCES})
# DuckDB already links miniz into the host. Bundle it only in the loadable
# extension, where host-internal symbols may not be exported.
build_loadable_extension(
  ${TARGET_NAME}
  " "
  ${EXTENSION_SOURCES}
  ${BZIP2_SOURCES}
  ${LZOKAY_SOURCE}
  ${LZ4_SOURCE}
  ${SNAPPY_SOURCES}
  "${DUCKDB_MINIZ_DIR}/miniz.cpp")
# DuckDB's Parquet headers expose a different, namespaced Brotli API. Always
# select the headers matching the libraries linked here.
target_include_directories(${EXTENSION_NAME} BEFORE
                           PRIVATE "${BROTLI_DIR}/c/include")
target_include_directories(${LOADABLE_EXTENSION_NAME} BEFORE
                           PRIVATE "${BROTLI_DIR}/c/include")
target_link_libraries(${EXTENSION_NAME} liblzma brotlienc brotlidec)
target_link_libraries(${LOADABLE_EXTENSION_NAME} liblzma brotlienc brotlidec)

install(
  TARGETS ${EXTENSION_NAME} liblzma brotlienc brotlidec brotlicommon
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

if(BUILD_UNITTESTS)
  enable_testing()
  add_executable(compression_fs_brotli_test test/brotli/decoder_regression.c)
  target_include_directories(compression_fs_brotli_test BEFORE
                             PRIVATE "${BROTLI_DIR}/c/include")
  target_include_directories(compression_fs_brotli_test
                             PRIVATE "${BROTLI_DIR}/c/dec")
  target_link_libraries(compression_fs_brotli_test brotlidec)
  add_test(NAME compression_fs_brotli_security
           COMMAND compression_fs_brotli_test)
  set_tests_properties(compression_fs_brotli_security PROPERTIES TIMEOUT 10)
endif()
