# 0.2.1

## Fixed

- Fix CVE-2026-111497 in the Brotli filesystem by upgrading to independently pinned Brotli 1.2.1 for both static and loadable extension builds.
- Fix duplicate miniz symbols when the loadable extension links against a prebuilt DuckDB static library.

# 0.2.0

## Updated

- Use DuckDB's allocator when growing LZO read buffers.
- Sync DuckDB to the latest `v2.0-cyanoptera` commit and extension-ci-tools to the latest `main` commit.

# 0.1.0

## Added

- Support LZ4, Brotli, Snappy, Bzip2, and XZ compression filesystems
