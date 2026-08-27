cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME compression_fs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED DUCKDB_MODULE_BASE_DIR)
  set(DUCKDB_LZ4_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/lz4")
  set(DUCKDB_SNAPPY_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/snappy")
else()
  set(DUCKDB_LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/lz4")
  set(DUCKDB_SNAPPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/snappy")
endif()

include_directories(src/include ${DUCKDB_LZ4_DIR} ${DUCKDB_SNAPPY_DIR})

set(EXTENSION_SOURCES
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
set(STATIC_SOURCES ${EXTENSION_SOURCES})
if(NOT DUCKDB_EXTENSION_PARQUET_SHOULD_LINK)
  list(APPEND STATIC_SOURCES ${LZ4_SOURCE} ${SNAPPY_SOURCES})
endif()

build_static_extension(${TARGET_NAME} ${STATIC_SOURCES})
build_loadable_extension(
  ${TARGET_NAME} " " ${EXTENSION_SOURCES} ${LZ4_SOURCE} ${SNAPPY_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
