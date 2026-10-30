# 0.2.0

## Updated

- Pin Brotli 1.2.1 independently of DuckDB to fix CVE-2026-111497 in the Brotli filesystem.

- Use DuckDB's allocator when growing LZO read buffers.
- Sync DuckDB to the latest `v2.0-cyanoptera` commit and extension-ci-tools to the latest `main` commit.

# 0.1.0

## Added

- Support LZ4, Brotli, Snappy, Bzip2, and XZ compression filesystems
