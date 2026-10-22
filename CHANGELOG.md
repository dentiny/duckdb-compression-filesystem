# 0.2.0

## Updated

- Reuse DuckDB's miniz when statically linking loadable extensions to avoid duplicate symbols.
- Use DuckDB's allocator when growing LZO read buffers.
- Sync DuckDB to the latest `v2.0-cyanoptera` commit and extension-ci-tools to the latest `main` commit.

# 0.1.0

## Added

- Support LZ4, Brotli, Snappy, Bzip2, and XZ compression filesystems
