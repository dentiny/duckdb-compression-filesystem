# DuckDB Compression Filesystem Extension

`compression_fs` adds file-level compression formats that DuckDB does not
support natively. It integrates with DuckDB's virtual filesystem, so compressed
files can be used directly by readers and writers such as `read_csv` and
`COPY`.

## Supported formats

| Format | Compression name | Auto-detected suffixes | Stream format |
| --- | --- | --- | --- |
| LZ4 | `lz4` | `.lz4` | LZ4 frame |
| Snappy | `snappy` | `.sz`, `.snappy` | Snappy framed |
| Brotli | `brotli` | `.br` | Brotli stream |

DuckDB already provides native file-level support for gzip and Zstandard.

The extension uses the LZ4, Snappy, and Brotli implementations vendored in the
DuckDB source tree; it does not add external runtime dependencies.

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

COPY events TO 'events.csv.sz'
    (FORMAT CSV, HEADER, COMPRESSION 'snappy');

COPY events TO 'events.csv.br'
    (FORMAT CSV, HEADER, COMPRESSION 'brotli');
```

### Read using suffix detection

```sql
SELECT * FROM read_csv('events.csv.lz4');
SELECT * FROM read_csv('events.csv.sz');
SELECT * FROM read_csv('events.csv.br');
```

### Read using an explicit compression type

Explicit compression is useful when the filename does not have a recognized
suffix:

```sql
SELECT * FROM read_csv('events.csv', compression = 'lz4');
SELECT * FROM read_csv('events.csv', compression = 'snappy');
SELECT * FROM read_csv('events.csv', compression = 'brotli');
```
