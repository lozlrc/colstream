// colstream: DEFLATE codec via zlib. Raw deflate streams (windowBits -15,
// no zlib/gzip wrapper), compression level 6.
#pragma once

#include <cstddef>
#include <cstdint>

namespace colstream {

// Worst-case compressed size for n input bytes.
size_t deflate_bound(size_t n);

// Returns compressed size, or 0 on failure (insufficient dst_cap).
size_t deflate_compress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap);

// Returns decompressed size, or -1 on malformed input or insufficient dst_cap.
long deflate_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap);

}  // namespace colstream
