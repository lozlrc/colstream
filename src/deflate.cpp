// DEFLATE codec via zlib. Raw deflate (deflateInit2 with windowBits -15, no
// zlib header or checksum), level 6, default memLevel and strategy. Streams
// are per-thread and reset between calls so no allocation happens per frame.
#include "colstream/deflate.hpp"

#include <zlib.h>

#include <stdexcept>

namespace colstream {
namespace {

struct DeflateCtx {
  z_stream s{};
  bool init = false;
  z_stream* get() {
    if (!init) {
      s.zalloc = Z_NULL;
      s.zfree = Z_NULL;
      s.opaque = Z_NULL;
      if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("colstream: deflateInit2 failed");
      init = true;
    }
    return &s;
  }
  ~DeflateCtx() {
    if (init) deflateEnd(&s);
  }
};

struct InflateCtx {
  z_stream s{};
  bool init = false;
  z_stream* get() {
    if (!init) {
      s.zalloc = Z_NULL;
      s.zfree = Z_NULL;
      s.opaque = Z_NULL;
      if (inflateInit2(&s, -15) != Z_OK)
        throw std::runtime_error("colstream: inflateInit2 failed");
      init = true;
    }
    return &s;
  }
  ~InflateCtx() {
    if (init) inflateEnd(&s);
  }
};

thread_local DeflateCtx g_deflate;
thread_local InflateCtx g_inflate;

}  // namespace

size_t deflate_bound(size_t n) { return deflateBound(g_deflate.get(), static_cast<uLong>(n)); }

size_t deflate_compress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap) {
  z_stream* s = g_deflate.get();
  deflateReset(s);
  s->next_in = const_cast<Bytef*>(src);
  s->avail_in = static_cast<uInt>(src_len);
  s->next_out = dst;
  s->avail_out = static_cast<uInt>(dst_cap);
  if (deflate(s, Z_FINISH) != Z_STREAM_END) return 0;
  return dst_cap - s->avail_out;
}

long deflate_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap) {
  z_stream* s = g_inflate.get();
  inflateReset(s);
  s->next_in = const_cast<Bytef*>(src);
  s->avail_in = static_cast<uInt>(src_len);
  // inflate(Z_FINISH) reports Z_BUF_ERROR with a zero-size output buffer even
  // when the stream decodes to nothing, so hand it one scratch byte instead.
  uint8_t scratch;
  const bool empty_out = dst_cap == 0;
  s->next_out = empty_out ? &scratch : dst;
  s->avail_out = empty_out ? 1 : static_cast<uInt>(dst_cap);
  if (inflate(s, Z_FINISH) != Z_STREAM_END) return -1;
  const size_t produced = (empty_out ? 1 : dst_cap) - s->avail_out;
  if (empty_out && produced > 0) return -1;  // stream had data, caller had no room
  return static_cast<long>(produced);
}

}  // namespace colstream
