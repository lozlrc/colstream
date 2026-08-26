// colstream: LZ4 block format codec, implemented from the public format
// specification. Independent code, wire-compatible with reference liblz4
// (cross-verified in tests/test_lz4_cross.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace colstream {

// Worst-case compressed size for n input bytes.
constexpr size_t lz4_compress_bound(size_t n) { return n + n / 255 + 16; }

// Compresses src[0..src_len) into dst. Requires dst_cap >=
// lz4_compress_bound(src_len); returns 0 if the capacity is short, otherwise
// the compressed size (>= 1). Incompressible input degrades to literal runs
// and never fails.
size_t lz4_compress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap);

// Decompresses one block. Returns the decompressed size, or -1 on malformed
// input (truncation, offset 0, offset beyond produced output, output
// overflow). Never reads past src + src_len or writes past dst + dst_cap.
long lz4_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap);

}  // namespace colstream
