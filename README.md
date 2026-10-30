# DuckDB Compression Filesystem Extension

`compression_fs` adds file-level compression formats that DuckDB does not
support natively. It integrates with DuckDB's virtual filesystem, so compressed
files can be used directly by readers and writers such as `read_csv` and
`COPY`.

## Supported formats

| Format | Compression name | Auto-detected suffixes | Stream format |
| --- | --- | --- | --- |
| LZ4 | `lz4` | `.lz4` | LZ4 frame |
| LZO | `lzo` | `.lzo` | lzop container (LZO1X) |
| Snappy | `snappy` | `.sz`, `.snappy` | Snappy framed |
| Brotli | `brotli` | `.br` | Brotli stream |
| Bzip2 | `bzip2` | `.bz2` | Bzip2 stream |
| DEFLATE | `deflate` | `.deflate` | zlib-wrapped DEFLATE (RFC 1950/1951) |
| LZMA | `lzma` | `.lzma` | Legacy LZMA-Alone stream |
| XZ | `xz` | `.xz` | XZ stream |

DuckDB already provides native file-level support for gzip and Zstandard.

The extension uses the LZ4, Snappy, and miniz implementations vendored in the
DuckDB source tree. Brotli 1.2.1, Bzip2 1.0.8, and XZ Utils 5.8.3 are included
as source submodules, so the extension does not require external runtime libraries.

Brotli is pinned independently of DuckDB to include the fix for
[CVE-2026-111497](https://github.com/google/brotli/releases/tag/v1.2.1).
Both static and loadable builds use this patched library for the `brotli`
filesystem. This does not replace the Brotli decoder used by DuckDB's Parquet
extension; keep DuckDB updated separately for that path.

`deflate` reads and writes zlib-wrapped streams, compatible with Hadoop
DefaultCodec and Python `zlib.compress`. Raw DEFLATE, gzip, ZIP containers,
and preset dictionaries are not supported by this codec.

LZO uses the MIT-licensed [lzokay](https://github.com/AxioDL/lzokay) source
submodule. It reads and writes lzop containers, including block checksums.
Raw/Hadoop LzoCodec blocks, filters, multipart headers, and extra header
fields are not supported. Blocks are limited to 64 MiB.

`lzma` uses the existing liblzma dependency to read and write legacy
LZMA-Alone files (as produced by `xz --format=lzma`). This format has no
integrity checksum; it is distinct from XZ and raw LZMA streams.

## Usage

```sql
LOAD 'build/release/extension/compression_fs/compression_fs.duckdb_extension';
```

### Write compressed CSV

```sql
CREATE TABLE events AS
SELECT i AS id, 'event-' || i::VARCHAR AS name
FROM range(10000) AS t(i);

COPY events TO 'events.csv.lz4'
    (FORMAT CSV, HEADER, COMPRESSION 'lz4');

COPY events TO 'events.csv.lzo'
    (FORMAT CSV, HEADER, COMPRESSION 'lzo');

COPY events TO 'events.csv.sz'
    (FORMAT CSV, HEADER, COMPRESSION 'snappy');

COPY events TO 'events.csv.br'
    (FORMAT CSV, HEADER, COMPRESSION 'brotli');

COPY events TO 'events.csv.bz2'
    (FORMAT CSV, HEADER, COMPRESSION 'bzip2');

COPY events TO 'events.csv.deflate'
    (FORMAT CSV, HEADER, COMPRESSION 'deflate');

COPY events TO 'events.csv.lzma'
    (FORMAT CSV, HEADER, COMPRESSION 'lzma');

COPY events TO 'events.csv.xz'
    (FORMAT CSV, HEADER, COMPRESSION 'xz');
```

### Read using suffix detection

```sql
SELECT * FROM read_csv('events.csv.lz4');
SELECT * FROM read_csv('events.csv.lzo');
SELECT * FROM read_csv('events.csv.sz');
SELECT * FROM read_csv('events.csv.br');
SELECT * FROM read_csv('events.csv.bz2');
SELECT * FROM read_csv('events.csv.deflate');
SELECT * FROM read_csv('events.csv.lzma');
SELECT * FROM read_csv('events.csv.xz');
```

### Read using an explicit compression type

Explicit compression is useful when the filename does not have a recognized
suffix:

```sql
SELECT * FROM read_csv('events.csv', compression = 'lz4');
SELECT * FROM read_csv('events.csv', compression = 'lzo');
SELECT * FROM read_csv('events.csv', compression = 'snappy');
SELECT * FROM read_csv('events.csv', compression = 'brotli');
SELECT * FROM read_csv('events.csv', compression = 'bzip2');
SELECT * FROM read_csv('events.csv', compression = 'deflate');
SELECT * FROM read_csv('events.csv', compression = 'lzma');
SELECT * FROM read_csv('events.csv', compression = 'xz');
```

## Brotli security regression

After configuring the extension build, run the decoder regression separately:

```sh
cmake --build build/release --target compression_fs_brotli_test
ctest --test-dir build/release/extension/compression_fs -R '^compression_fs_brotli_security$' --output-on-failure
```

The test constructs the exhausted single-type block state behind
CVE-2026-111497 and checks that short-input and fast-input decoding return a
terminal format error. It uses Brotli's private state layout from the pinned
submodule. Filesystem round trips, buffer boundaries, and malformed input are
covered by `test/sql/test_brotli_compress_fs.test`.
