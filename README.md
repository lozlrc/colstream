# colstream

[![ci](https://github.com/lozlrc/colstream/actions/workflows/ci.yml/badge.svg)](https://github.com/lozlrc/colstream/actions/workflows/ci.yml)

Columnar delta feed: a publisher maintains a typed table, streams snapshots and
deltas as compact frames, and a subscriber rebuilds identical state from an
arbitrarily fragmented byte stream. Three payload layouts and three codecs,
benchmarked against each other.

Inspired by the message-compression problem described in HRT's 2025 SWE intern
spotlight (https://www.hudsonrivertrading.com/hrtbeat/intern-spotlight-2025-software-engineering-summer-projects/),
but this is an independent from-scratch implementation with no affiliation.

The library is C++20 with zero dependencies (std + POSIX; zlib for the DEFLATE
codec only). Reference liblz4 is linked into one cross-verification test binary
and nothing else.

## Design

Rows are instrument slots addressed by a dense u32 index, fixed at
construction. Columns are runtime-typed (i64 or f64) with optional per-column
nullability backed by validity bitmaps. Storage is column-major contiguous
arrays of raw 64-bit patterns, so f64 NaN payloads and signed zeros survive
round trips bit-exact.

The publisher tracks per-column dirty row lists as writes happen (a bitmap
dedups repeated writes to the same cell). `publish_delta()` builds the payload
for the chosen layout into a reusable buffer, compresses into a second reusable
buffer, and returns a span over the finished frame. `publish_snapshot()` emits
full state through the same builders and also clears the dirty set, since a
snapshot carries everything. The subscriber (`Feed::on_bytes`) accepts any
fragmentation, buffers partial frames, decompresses, applies to its own table,
and flags sequence gaps.

    Publisher                                          Feed (subscriber)
    +--------------------+                             +--------------------+
    | Table (col-major)  |   frames: snapshot/delta    | Table (col-major)  |
    | dirty rows per col | --> [hdr|payload] ------->  | partial-frame buf  |
    | build+compress buf |     any byte fragmentation  | decompress+apply   |
    +--------------------+                             +--------------------+

Rules are publisher-authoritative by construction: the wire carries cell
values, not operations, so a subscriber that applies every frame in sequence
holds a bit-exact replica.

## Wire format

All integers little-endian. Frame:

| field       | type | notes                                   |
|-------------|------|-----------------------------------------|
| payload_len | u32  | payload bytes only, header excluded     |
| msg_type    | u8   | 0 snapshot, 1 delta                     |
| layout      | u8   | 0 CELLWISE, 1 COL_RAW, 2 COL_PACKED     |
| codec       | u8   | 0 NONE, 1 LZ4, 2 DEFLATE                |
| seq         | u64  | one counter shared by both message types |
| payload     |      | see below                               |

With codec != NONE the payload is `[u32 raw_len][compressed bytes]`. A
snapshot uses the same layout encodings with every row of every column
present; the subscriber resets its table before applying one.

Layouts (payload after decompression):

| layout     | encoding                                                              |
|------------|-----------------------------------------------------------------------|
| CELLWISE   | sequence of 14-byte cells: u32 row, u16 col, u64 raw value            |
| COL_RAW    | u16 block count, then per column block: u16 col, row-set, [null bitmask], raw u64 per non-null row |
| COL_PACKED | as COL_RAW but values are packed (scheme below)                       |

CELLWISE reserves the top bit of the col field as the null flag; a null cell
still carries 8 value bytes, all zero. Column blocks appear only for columns
with changes (all columns in a snapshot). The null bitmask appears only for
nullable columns: ceil(k/8) bytes over the k row-set entries, LSB-first, bit
set means null, and null rows carry no value bytes.

Row-set: 1 selector byte, then

| selector | encoding                                                        |
|----------|-----------------------------------------------------------------|
| 0        | raw bitset over nrows, LSB-first, ceil(nrows/8) bytes           |
| 1        | varint count, varint first row, varint gaps (row[i] - row[i-1]) |

The publisher computes both sizes and emits the smaller (bitset on ties).
Varints are LEB128.

Packed values: i64 is zigzag-mapped to u64; f64 is the byteswapped bit
pattern, which turns the zero low mantissa bytes of price-like doubles into
leading zero bytes. Each u64 key is then written as its minimal little-endian
byte count (0 to 8). The lengths are 4-bit nibbles packed two per byte (even
index in the low nibble, odd in the high, zero-padded to a whole byte), all
nibbles first, then all value bytes concatenated.

## LZ4 implementation notes

`src/lz4.cpp` implements the LZ4 block format from the public specification:
token byte with literal length in the high nibble and match length minus 4 in
the low nibble, 255-extension bytes after a nibble of 15, 2-byte little-endian
offsets in 1..65535, minimum match 4. The encoder honors the end-of-block
rules: the last 5 bytes are always literals and no match starts within the
last 12 bytes, so every block it emits is acceptable to fast reference
decoders. The compressor is a single-probe 4096-entry hash table (4-byte keys,
multiplicative hash) over the 64 KiB window, with the step-skipping
acceleration of the fast reference path and back-to-back match chaining.
`lz4_compress_bound(n) = n + n/255 + 16`; incompressible input degrades to
literal runs and never fails.

The decoder is a safe decoder: it never reads or writes out of bounds and
returns -1 on truncated input, offset 0, offsets reaching before the start of
output, or output overflow. Golden vectors, malformed-input cases, and a
2000-case fuzz run in `tests/test_main.cpp`; `tests/test_lz4_cross.cpp`
verifies 500 seeded cases in both directions against reference liblz4
(compress here, decompress there, and vice versa).

DEFLATE is zlib with raw streams (`deflateInit2` windowBits -15, no wrapper),
level 6, with per-thread streams reset between frames so steady state does no
allocation.

## Benchmarks

Apple M2 Pro, macOS, clang 17, -O2. Synthetic market table, 5000 rows x 12
columns (4 f64 prices with 2 to 4 decimal places, 8 i64 sizes and counters),
zipf(1.2) row updates so hot instruments dominate, 200 updates per delta, 5000
deltas per combo, first 500 excluded as warmup. Reproduce with `make bench`;
the run also writes `bench/results.txt`.

| layout     | codec   | payload B/msg | ratio | pub ns/msg | sub ns/msg |
|------------|---------|--------------:|------:|-----------:|-----------:|
| CELLWISE   | NONE    |        9391.4 | 1.000 |      11732 |       1573 |
| CELLWISE   | LZ4     |        5947.0 | 0.633 |      30786 |      13710 |
| CELLWISE   | DEFLATE |        3975.0 | 0.423 |     299094 |      18958 |
| COL_RAW    | NONE    |        6200.9 | 0.660 |       9762 |       2882 |
| COL_RAW    | LZ4     |        3525.5 | 0.375 |      21085 |       9144 |
| COL_RAW    | DEFLATE |        2624.1 | 0.279 |     164580 |      15508 |
| COL_PACKED | NONE    |        4390.3 | 0.467 |      11557 |       4856 |
| COL_PACKED | LZ4     |        3047.9 | 0.325 |      21750 |       8271 |
| COL_PACKED | DEFLATE |        2609.9 | 0.278 |      99773 |      16941 |

Snapshot payload bytes, same table:

| layout     |    NONE |     LZ4 | DEFLATE |
|------------|--------:|--------:|--------:|
| CELLWISE   |  840000 |  520617 |  297446 |
| COL_RAW    |  487538 |  251522 |  164777 |
| COL_PACKED |  296618 |  209527 |  158240 |

Reading of this run: the columnar layouts alone remove a third to a half of
the baseline bytes at no codec cost. LZ4 buys another 25 to 30 points of
ratio for about 10 us per message on each side. DEFLATE compresses best but
costs 5x to 15x the publisher time of LZ4; past COL_PACKED + LZ4 it mostly
buys back bytes the packing already removed.

## Tests

`make test` builds and runs everything. `tests/test_main.cpp` covers varint
and packing edge cases (INT64_MIN/MAX, NaN payloads, denormals, signed zero,
nibble parity), row-set round trips and selector choice, LZ4 golden vectors
verified by hand, malformed-input rejection, round trips from empty through
incompressible 100 KiB, a 2000-case seeded fuzz, deflate round trips, and an
end-to-end run: 10k seeded mutations on a 2000x10 table with nullable columns,
published as deltas plus periodic snapshots across all nine layout x codec
combos, delivered through a fragmenter that slices the stream at random 1..7000
byte boundaries, with bit-exact table comparison after every message, plus
sequence-gap detection. `tests/test_lz4_cross.cpp` cross-verifies the LZ4
codec against reference liblz4 when `/opt/homebrew/include/lz4.h` exists;
otherwise `make test` prints a SKIP line and the rest of the suite still runs.

## Limitations

- Single-threaded; one publisher, one table per Feed.
- No transport included beyond byte-stream framing; point it at TCP, a pipe,
  or a file.
- Fixed row universe: the row count is set at construction and rows cannot be
  added or removed, only overwritten.
- i64 and f64 columns only.
- The subscriber throws on malformed frames rather than resynchronizing; gap
  recovery (request a fresh snapshot) is left to the caller.
