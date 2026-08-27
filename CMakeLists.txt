cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME compression_fs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED DUCKDB_MODULE_BASE_DIR)
  set(DUCKDB_LZ4_DIR "${DUCKDB_MODULE_BASE_DIR}/third_party/lz4")
else()
  set(DUCKDB_LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/lz4")
endif()

include_directories(src/include ${DUCKDB_LZ4_DIR})

set(EXTENSION_SOURCES
    src/compression_fs_extension.cpp
    src/lz4_frame_utils.cpp
    src/lz4_file_system.cpp)

# In-tree LZ4 (duckdb/third_party/lz4). The parquet static library already compiles
# lz4.cpp, so only add it here when parquet is not being linked into the same binary.
set(LZ4_SOURCE "${DUCKDB_LZ4_DIR}/lz4.cpp")
set(STATIC_SOURCES ${EXTENSION_SOURCES})
if(NOT DUCKDB_EXTENSION_PARQUET_SHOULD_LINK)
  list(APPEND STATIC_SOURCES ${LZ4_SOURCE})
endif()

build_static_extension(${TARGET_NAME} ${STATIC_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES} ${LZ4_SOURCE})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
