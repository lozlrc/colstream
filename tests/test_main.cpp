// colstream test suite. Assert-style checks, no framework.
#include <bit>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "colstream/deflate.hpp"
#include "colstream/frame.hpp"
#include "colstream/lz4.hpp"
#include "colstream/pack.hpp"
#include "colstream/publisher.hpp"
#include "colstream/subscriber.hpp"

using namespace colstream;

static int g_checks = 0;
static int g_total = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      std::abort();                                                         \
    }                                                                       \
    g_checks++;                                                             \
  } while (0)

static void section_done(const char* name) {
  std::printf("PASS %-28s %6d checks\n", name, g_checks);
  g_total += g_checks;
  g_checks = 0;
}

// Deterministic RNG (splitmix64).
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

// ---------------------------------------------------------------- varint

static void test_varint() {
  const uint64_t cases[] = {0, 1, 127, 128, 129, 16383, 16384, 1u << 20, 0xFFFFFFFFull,
                            uint64_t{1} << 62, UINT64_MAX};
  for (uint64_t v : cases) {
    std::vector<uint8_t> b;
    put_varint(b, v);
    CHECK(b.size() == varint_len(v));
    uint64_t out = 1;
    const uint8_t* p = get_varint(b.data(), b.data() + b.size(), out);
    CHECK(p == b.data() + b.size());
    CHECK(out == v);
  }
  // Truncation and overflow are rejected.
  std::vector<uint8_t> t = {0x80, 0x80};
  uint64_t out;
  CHECK(get_varint(t.data(), t.data() + t.size(), out) == nullptr);
  std::vector<uint8_t> ov(10, 0xFF);
  ov.push_back(0x01);  // 11 groups, shift past 63
  CHECK(get_varint(ov.data(), ov.data() + ov.size(), out) == nullptr);
  section_done("varint");
}

// ---------------------------------------------------------------- packing

static void roundtrip_keys(const std::vector<uint64_t>& keys) {
  std::vector<uint8_t> buf;
  pack_values(buf, keys.data(), keys.size());
  std::vector<uint64_t> out(keys.size(), 0xABABABABABABABABull);
  const uint8_t* p = unpack_values(buf.data(), buf.data() + buf.size(), keys.size(), out.data());
  CHECK(p == buf.data() + buf.size());
  CHECK(out == keys);
}

static void test_packing() {
  // i64 edge cases through zigzag.
  const int64_t ivals[] = {0,       -1,        1,        INT64_MIN, INT64_MAX, 2,     -2,
                           63,      64,        -64,      -65,       1 << 20,   -(1 << 20),
                           int64_t{1} << 32,   -(int64_t{1} << 32), int64_t{1} << 62};
  for (int64_t v : ivals) {
    CHECK(zigzag_dec(zigzag_enc(v)) == v);
  }
  CHECK(zigzag_enc(0) == 0);
  CHECK(zigzag_enc(-1) == 1);
  CHECK(zigzag_enc(1) == 2);
  CHECK(zigzag_enc(INT64_MIN) == UINT64_MAX);
  CHECK(min_bytes(0) == 0);
  CHECK(min_bytes(1) == 1);
  CHECK(min_bytes(255) == 1);
  CHECK(min_bytes(256) == 2);
  CHECK(min_bytes(UINT64_MAX) == 8);
  {
    std::vector<uint64_t> keys;
    for (int64_t v : ivals) keys.push_back(zigzag_enc(v));
    roundtrip_keys(keys);
  }

  // f64: bit patterns must survive exactly, NaN payloads included.
  const uint64_t fbits[] = {
      0x0000000000000000ull,  // 0.0
      0x8000000000000000ull,  // -0.0
      0x3FF8000000000000ull,  // 1.5
      0x4059500000000000ull,  // 101.25
      0x7FF8000000000000ull,  // quiet NaN
      0x7FF8DEADBEEF1234ull,  // NaN with payload
      0xFFF0000000000001ull,  // negative NaN-space pattern
      0x7FF0000000000000ull,  // +inf
      0xFFF0000000000000ull,  // -inf
      0x0000000000000001ull,  // smallest denormal
      0x000FFFFFFFFFFFFFull,  // largest denormal
      0x4197D78400000000ull,  // 100000000.0-ish price
  };
  std::vector<uint64_t> fkeys;
  for (uint64_t b : fbits) {
    CHECK(f64_unkey(f64_key(b)) == b);
    fkeys.push_back(f64_key(b));
  }
  roundtrip_keys(fkeys);
  // Price-like doubles byteswap into short keys.
  CHECK(min_bytes(f64_key(std::bit_cast<uint64_t>(101.25))) == 3);
  CHECK(min_bytes(f64_key(std::bit_cast<uint64_t>(0.0))) == 0);

  // Nibble parity: odd and even counts, zero-length values interleaved.
  Rng rng(7);
  for (size_t n : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{7}, size_t{9},
                   size_t{100}, size_t{101}}) {
    std::vector<uint64_t> keys;
    for (size_t i = 0; i < n; i++) {
      unsigned bytes = rng.below(9);
      uint64_t k = rng.next();
      k = bytes == 0 ? 0 : (bytes == 8 ? (k | (uint64_t{1} << 63)) : k % (uint64_t{1} << (8 * bytes)));
      keys.push_back(k);
    }
    roundtrip_keys(keys);
  }
  // Exact size: two values, lengths 1 and 3 -> 1 nibble byte + 4 value bytes.
  {
    std::vector<uint8_t> buf;
    const uint64_t keys[2] = {0x7F, 0x030201};
    pack_values(buf, keys, 2);
    CHECK(buf.size() == 5);
    CHECK(buf[0] == 0x31);  // low nibble len 1 (index 0), high nibble len 3 (index 1)
    CHECK(buf[1] == 0x7F && buf[2] == 0x01 && buf[3] == 0x02 && buf[4] == 0x03);
  }
  // Malformed: truncated value bytes, truncated nibbles, nibble > 8.
  {
    std::vector<uint8_t> buf = {0x09};  // one value, claimed length 9
    uint64_t k;
    CHECK(unpack_values(buf.data(), buf.data() + buf.size(), 1, &k) == nullptr);
    std::vector<uint8_t> buf2 = {0x02, 0xAA};  // length 2, one byte present
    CHECK(unpack_values(buf2.data(), buf2.data() + buf2.size(), 1, &k) == nullptr);
    std::vector<uint8_t> buf3;
    CHECK(unpack_values(buf3.data(), buf3.data(), 1, &k) == nullptr);
  }
  section_done("packing");
}

// ---------------------------------------------------------------- row-set

static void roundtrip_rowset(const std::vector<uint32_t>& rows, uint32_t nrows, int want_sel) {
  std::vector<uint8_t> buf;
  rowset_encode(buf, rows.data(), rows.size(), nrows);
  if (want_sel >= 0) CHECK(buf[0] == static_cast<uint8_t>(want_sel));
  std::vector<uint32_t> out;
  const uint8_t* p = rowset_decode(buf.data(), buf.data() + buf.size(), nrows, out);
  CHECK(p == buf.data() + buf.size());
  CHECK(out == rows);
}

static void test_rowset() {
  roundtrip_rowset({}, 1024, 1);            // empty list beats 129-byte bitset
  roundtrip_rowset({0}, 1024, 1);
  roundtrip_rowset({1023}, 1024, 1);
  roundtrip_rowset({3, 500, 501, 999}, 1024, 1);
  {
    std::vector<uint32_t> dense;
    for (uint32_t r = 0; r < 900; r++) dense.push_back(r);
    roundtrip_rowset(dense, 1024, 0);       // 900 varints lose to 129-byte bitset
  }
  {
    std::vector<uint32_t> all;
    for (uint32_t r = 0; r < 64; r++) all.push_back(r);
    roundtrip_rowset(all, 64, 0);
  }
  roundtrip_rowset({0, 1, 2}, 8, -1);       // tiny table, either encoding is fine
  {
    // Selector picks by exact size: 5 rows in 64 -> list is 7 bytes, bitset 9.
    std::vector<uint8_t> buf;
    const std::vector<uint32_t> rows = {1, 9, 17, 33, 63};
    rowset_encode(buf, rows.data(), rows.size(), 64);
    CHECK(buf[0] == 1 && buf.size() == 7);
    // 20 rows in 64 -> list would be 22 bytes, bitset 9 wins.
    std::vector<uint32_t> rows2;
    for (uint32_t r = 0; r < 20; r++) rows2.push_back(r * 3);
    buf.clear();
    rowset_encode(buf, rows2.data(), rows2.size(), 64);
    CHECK(buf[0] == 0 && buf.size() == 9);
  }
  // Malformed inputs are rejected.
  {
    std::vector<uint32_t> out;
    const uint8_t sel2[] = {2};
    CHECK(rowset_decode(sel2, sel2 + 1, 8, out) == nullptr);
    const uint8_t stray[] = {0, 0x00, 0x80};  // bit 15 set, nrows 10
    out.clear();
    CHECK(rowset_decode(stray, stray + 3, 10, out) == nullptr);
    const uint8_t nonasc[] = {1, 2, 5, 0};  // gap 0
    out.clear();
    CHECK(rowset_decode(nonasc, nonasc + 4, 100, out) == nullptr);
    const uint8_t oob[] = {1, 1, 200};  // row 200 in nrows 100
    out.clear();
    CHECK(rowset_decode(oob, oob + 3, 100, out) == nullptr);
    const uint8_t trunc[] = {1, 3, 1};  // claims 3 rows, one present
    out.clear();
    CHECK(rowset_decode(trunc, trunc + 3, 100, out) == nullptr);
    const uint8_t truncbits[] = {0, 0xFF};  // nrows 64 needs 8 bitset bytes
    out.clear();
    CHECK(rowset_decode(truncbits, truncbits + 2, 64, out) == nullptr);
  }
  section_done("row-set");
}

// ---------------------------------------------------------------- LZ4

static void lz4_roundtrip(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> comp(lz4_compress_bound(in.size()));
  const size_t c = lz4_compress(in.data(), in.size(), comp.data(), comp.size());
  CHECK(c >= 1 && c <= comp.size());
  std::vector<uint8_t> out(in.size());
  const long n = lz4_decompress(comp.data(), c, out.data(), out.size());
  CHECK(n == static_cast<long>(in.size()));
  CHECK(std::memcmp(out.data(), in.data(), in.size()) == 0);
}

static void test_lz4_golden() {
  {  // empty input -> single empty-literals token
    uint8_t dst[16];
    CHECK(lz4_compress(nullptr, 0, dst, sizeof dst) == 1);
    CHECK(dst[0] == 0x00);
    uint8_t out[4];
    CHECK(lz4_decompress(dst, 1, out, sizeof out) == 0);
  }
  {  // short input: all literals, no match attempted
    const uint8_t in[] = {'a', 'b', 'c', 'd', 'e'};
    uint8_t dst[32];
    const size_t c = lz4_compress(in, 5, dst, sizeof dst);
    const uint8_t want[] = {0x50, 'a', 'b', 'c', 'd', 'e'};
    CHECK(c == 6 && std::memcmp(dst, want, 6) == 0);
  }
  {  // 16 x 'a': one literal, offset-1 match of 10, five trailing literals
    std::vector<uint8_t> in(16, 'a');
    uint8_t dst[64];
    const size_t c = lz4_compress(in.data(), in.size(), dst, sizeof dst);
    const uint8_t want[] = {0x16, 'a', 0x01, 0x00, 0x50, 'a', 'a', 'a', 'a', 'a'};
    CHECK(c == 10 && std::memcmp(dst, want, 10) == 0);
    uint8_t out[16];
    CHECK(lz4_decompress(dst, c, out, 16) == 16);
    CHECK(std::memcmp(out, in.data(), 16) == 0);
  }
  {  // decode-only: overlapping offset-1 match
    const uint8_t src[] = {0x22, 'x', 'y', 0x01, 0x00, 0x10, 'z'};
    uint8_t out[16];
    CHECK(lz4_decompress(src, sizeof src, out, sizeof out) == 9);
    CHECK(std::memcmp(out, "xyyyyyyyz", 9) == 0);
  }
  {  // decode-only: literal length extension 15 + 2
    std::vector<uint8_t> src = {0xF0, 0x02};
    for (int i = 0; i < 17; i++) src.push_back(static_cast<uint8_t>('A' + i));
    uint8_t out[32];
    CHECK(lz4_decompress(src.data(), src.size(), out, sizeof out) == 17);
    CHECK(std::memcmp(out, src.data() + 2, 17) == 0);
  }
  {  // decode-only: match length extension 19 + 5
    const uint8_t src[] = {0x1F, 'a', 0x01, 0x00, 0x05, 0x10, 'b'};
    uint8_t out[64];
    CHECK(lz4_decompress(src, sizeof src, out, sizeof out) == 26);
    for (int i = 0; i < 25; i++) CHECK(out[i] == 'a');
    CHECK(out[25] == 'b');
  }
  section_done("lz4 golden");
}

static void test_lz4_malformed() {
  uint8_t out[64];
  const auto rej = [&](std::vector<uint8_t> src, size_t cap = sizeof(out)) {
    CHECK(lz4_decompress(src.data(), src.size(), out, cap) == -1);
  };
  rej({});                                   // empty stream
  rej({0x10});                               // truncated literals
  rej({0x50, 'a', 'b', 'c'});                // literal run past input end
  rej({0x10, 'x', 0x01});                    // truncated offset
  rej({0x10, 'x', 0x00, 0x00});              // offset 0
  rej({0x10, 'x', 0x05, 0x00});              // offset beyond produced output
  rej({0xF0});                               // truncated literal-length extension
  rej({0xF0, 0xFF});                         // extension never terminates
  rej({0x1F, 'x', 0x01, 0x00});              // truncated match-length extension
  rej({0x22, 'x', 'y', 0x01, 0x00});         // stream ends on a match, not literals
  rej({0x50, 'a', 'b', 'c', 'd', 'e'}, 3);   // output buffer too small for literals
  rej({0x1F, 'a', 0x01, 0x00, 0x00}, 5);     // match overruns output buffer
  // Truncating a valid stream at every point must fail or yield a prefix
  // decode, never crash. (Prefix decodes happen when the cut lands exactly
  // after a completed literal run.)
  std::vector<uint8_t> in(400);
  for (size_t i = 0; i < in.size(); i++) in[i] = static_cast<uint8_t>((i * 7) & 0x3F);
  std::vector<uint8_t> comp(lz4_compress_bound(in.size()));
  const size_t c = lz4_compress(in.data(), in.size(), comp.data(), comp.size());
  std::vector<uint8_t> big(in.size());
  for (size_t cut = 0; cut < c; cut++) {
    const long n = lz4_decompress(comp.data(), cut, big.data(), big.size());
    CHECK(n <= static_cast<long>(in.size()));
    if (n >= 0) CHECK(std::memcmp(big.data(), in.data(), static_cast<size_t>(n)) == 0);
  }
  section_done("lz4 malformed");
}

static void test_lz4_roundtrip() {
  lz4_roundtrip({});
  lz4_roundtrip({0x42});
  lz4_roundtrip(std::vector<uint8_t>(100 * 1024, 0));  // all zeros
  {
    Rng rng(11);
    std::vector<uint8_t> in(100 * 1024);
    for (auto& b : in) b = static_cast<uint8_t>(rng.next());
    lz4_roundtrip(in);  // incompressible: degrades to literals, never fails
    std::vector<uint8_t> comp(lz4_compress_bound(in.size()));
    const size_t c = lz4_compress(in.data(), in.size(), comp.data(), comp.size());
    CHECK(c <= lz4_compress_bound(in.size()));
    CHECK(c >= in.size());  // random data does not compress
  }
  {
    std::vector<uint8_t> in;
    while (in.size() < 100 * 1024) {
      in.push_back('a');
      in.push_back('b');
      in.push_back('c');
    }
    lz4_roundtrip(in);
    std::vector<uint8_t> comp(lz4_compress_bound(in.size()));
    const size_t c = lz4_compress(in.data(), in.size(), comp.data(), comp.size());
    CHECK(c < in.size() / 20);  // highly repetitive input compresses hard
  }
  {
    // Pseudo market data: 16-byte records, slowly moving ids and prices.
    Rng rng(13);
    std::vector<uint8_t> in;
    uint64_t px = 1012500;
    uint64_t ts = 1724232000000000000ull;
    for (int i = 0; i < 8000; i++) {
      px += rng.below(5) - 2;
      ts += 1000 + rng.below(500);
      for (int b = 0; b < 8; b++) in.push_back(static_cast<uint8_t>(px >> (8 * b)));
      for (int b = 0; b < 8; b++) in.push_back(static_cast<uint8_t>(ts >> (8 * b)));
    }
    lz4_roundtrip(in);
  }
  section_done("lz4 roundtrip");
}

static void test_lz4_fuzz() {
  Rng rng(0xC01D57EA);
  std::vector<uint8_t> in, comp, out;
  for (int iter = 0; iter < 2000; iter++) {
    const uint32_t szpick = rng.below(4);
    const size_t n = szpick == 0   ? rng.below(64)
                     : szpick == 1 ? rng.below(1024)
                     : szpick == 2 ? rng.below(8 * 1024)
                                   : rng.below(64 * 1024);
    in.resize(n);
    const uint32_t mode = rng.below(5);
    if (mode == 0) {  // pure random
      for (auto& b : in) b = static_cast<uint8_t>(rng.next());
    } else if (mode == 1) {  // small alphabet
      const uint32_t alpha = 2 + rng.below(6);
      for (auto& b : in) b = static_cast<uint8_t>(rng.below(alpha));
    } else if (mode == 2) {  // runs of repeated bytes
      size_t i = 0;
      while (i < n) {
        const uint8_t v = static_cast<uint8_t>(rng.next());
        size_t run = 1 + rng.below(300);
        while (run-- && i < n) in[i++] = v;
      }
    } else if (mode == 3) {  // mostly zeros, sparse noise
      std::fill(in.begin(), in.end(), 0);
      for (size_t k = 0; k < n / 16; k++) in[rng.below(static_cast<uint32_t>(n))] =
          static_cast<uint8_t>(rng.next());
    } else {  // mixed blocks: random chunk then repeated chunk
      size_t i = 0;
      while (i < n) {
        const bool rnd = rng.below(2) == 0;
        size_t blk = 1 + rng.below(512);
        const uint8_t v = static_cast<uint8_t>(rng.next());
        while (blk-- && i < n) in[i++] = rnd ? static_cast<uint8_t>(rng.next()) : v;
      }
    }
    comp.resize(lz4_compress_bound(n));
    const size_t c = lz4_compress(in.data(), n, comp.data(), comp.size());
    CHECK(c >= 1 && c <= comp.size());
    out.assign(n, 0xEE);
    const long d = lz4_decompress(comp.data(), c, out.data(), n);  // exact-size output
    CHECK(d == static_cast<long>(n));
    CHECK(in == out);
  }
  section_done("lz4 fuzz x2000");
}

// ---------------------------------------------------------------- deflate

static void test_deflate() {
  Rng rng(21);
  for (size_t n : {size_t{0}, size_t{1}, size_t{100}, size_t{4096}, size_t{70000}}) {
    std::vector<uint8_t> in(n);
    for (size_t i = 0; i < n; i++)
      in[i] = (i % 3 == 0) ? static_cast<uint8_t>(rng.next()) : static_cast<uint8_t>(i & 0x0F);
    std::vector<uint8_t> comp(deflate_bound(n));
    const size_t c = deflate_compress(in.data(), n, comp.data(), comp.size());
    CHECK(c > 0);
    std::vector<uint8_t> out(n);
    CHECK(deflate_decompress(comp.data(), c, out.data(), n) == static_cast<long>(n));
    CHECK(in == out);
  }
  uint8_t out[8];
  const uint8_t junk[] = {0xAB, 0xCD, 0xEF, 0x01};
  CHECK(deflate_decompress(junk, sizeof junk, out, sizeof out) == -1);
  section_done("deflate");
}

// ---------------------------------------------------------------- end-to-end

static Schema make_schema() {
  return Schema({
      {"px_bid", Type::F64, false},
      {"px_ask", Type::F64, false},
      {"px_last", Type::F64, true},
      {"vwap", Type::F64, true},
      {"bid_sz", Type::I64, false},
      {"ask_sz", Type::I64, false},
      {"volume", Type::I64, false},
      {"trade_ct", Type::I64, false},
      {"ts_ns", Type::I64, false},
      {"halt_code", Type::I64, true},
  });
}

static void mutate(Publisher& pub, Rng& rng, uint32_t nrows) {
  const uint32_t row = rng.below(nrows);
  const uint16_t col = static_cast<uint16_t>(rng.below(10));
  const Schema& sc = pub.table().schema();
  if (sc.col(col).nullable && rng.below(100) < 15) {
    pub.set_null(row, col);
    return;
  }
  if (sc.col(col).type == Type::I64) {
    static const int64_t pool[] = {0, -1, 1, INT64_MIN, INT64_MAX, 100, -100, 1 << 30};
    const uint32_t pick = rng.below(12);
    pub.set_i64(row, col, pick < 8 ? pool[pick] : static_cast<int64_t>(rng.next()));
  } else {
    static const uint64_t pool[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x4059500000000000ull,
        0x7FF8DEADBEEF1234ull, 0x7FF8000000000000ull, 0x0000000000000001ull,
        0x7FF0000000000000ull, 0xFFF0000000000000ull};
    const uint32_t pick = rng.below(12);
    const uint64_t bits =
        pick < 8 ? pool[pick]
                 : std::bit_cast<uint64_t>(100.0 + static_cast<double>(rng.below(400000)) * 0.0025);
    pub.set_f64(row, col, std::bit_cast<double>(bits));
  }
}

// Delivers queued bytes in random chunks of 1..7000 bytes.
static void drain(std::vector<uint8_t>& stream, Feed& sub, Rng& rng) {
  size_t off = 0;
  while (off < stream.size()) {
    const size_t chunk = std::min<size_t>(1 + rng.below(7000), stream.size() - off);
    sub.on_bytes(stream.data() + off, chunk);
    off += chunk;
  }
  stream.clear();
}

static void push(std::vector<uint8_t>& stream, std::span<const uint8_t> frame) {
  stream.insert(stream.end(), frame.begin(), frame.end());
}

static void test_end_to_end() {
  const uint32_t nrows = 2000;
  const Layout layouts[] = {Layout::Cellwise, Layout::ColRaw, Layout::ColPacked};
  const Codec codecs[] = {Codec::None, Codec::Lz4, Codec::Deflate};
  for (Layout layout : layouts) {
    for (Codec codec : codecs) {
      Publisher pub(make_schema(), nrows);
      Feed sub(make_schema(), nrows);
      Rng rng(0xFEED0001);  // identical mutation stream for every combo
      std::vector<uint8_t> stream;

      push(stream, pub.publish_snapshot(layout, codec));
      drain(stream, sub, rng);
      CHECK(sub.table().equals(pub.table()));

      for (int batch = 0; batch < 50; batch++) {
        for (int m = 0; m < 200; m++) mutate(pub, rng, nrows);
        push(stream, pub.publish_delta(layout, codec));
        if (batch % 10 == 7) {
          // Two frames queued at once: chunks straddle frame boundaries.
          for (int m = 0; m < 50; m++) mutate(pub, rng, nrows);
          push(stream, pub.publish_snapshot(layout, codec));
        }
        drain(stream, sub, rng);
        CHECK(sub.table().equals(pub.table()));
        CHECK(!sub.seq_gap());
      }
      // Empty delta is valid and applies as a no-op.
      push(stream, pub.publish_delta(layout, codec));
      drain(stream, sub, rng);
      CHECK(sub.table().equals(pub.table()));
      CHECK(pub.dirty_cells() == 0);
    }
  }
  section_done("end-to-end 3x3");
}

static void test_seq_gap() {
  const uint32_t nrows = 16;
  Publisher pub(make_schema(), nrows);
  Feed sub(make_schema(), nrows);
  Rng rng(0xBADC0FFE);
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 3; i++) {
    for (int m = 0; m < 20; m++) mutate(pub, rng, nrows);
    auto f = pub.publish_delta(Layout::ColPacked, Codec::Lz4);
    frames.emplace_back(f.begin(), f.end());
  }
  sub.on_bytes(frames[0].data(), frames[0].size());
  CHECK(!sub.seq_gap());
  // Drop frame 1: the gap must be flagged.
  sub.on_bytes(frames[2].data(), frames[2].size());
  CHECK(sub.seq_gap());
  CHECK(sub.gap_count() == 1);
  section_done("seq gap");
}

static void test_stream_hygiene() {
  const uint32_t nrows = 64;
  Publisher pub(make_schema(), nrows);
  Feed sub(make_schema(), nrows);
  Rng rng(0x51DE);
  for (int m = 0; m < 100; m++) mutate(pub, rng, nrows);
  auto f = pub.publish_delta(Layout::ColRaw, Codec::Deflate);
  // Byte at a time.
  for (size_t i = 0; i < f.size(); i++) sub.on_bytes(f.data() + i, 1);
  CHECK(sub.table().equals(pub.table()));
  CHECK(sub.frames_applied() == 1);

  // Corrupt header (payload_len over the cap) throws.
  Feed sub2(make_schema(), nrows);
  uint8_t junk[kFrameHeaderSize] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0};
  bool threw = false;
  try {
    sub2.on_bytes(junk, sizeof junk);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);

  // Hand-built frame whose LZ4 payload is invalid (offset 0) throws.
  Feed sub3(make_schema(), nrows);
  const uint8_t body[] = {0x10, 'x', 0x00, 0x00};
  std::vector<uint8_t> bad(kFrameHeaderSize + 4 + sizeof body);
  FrameHeader h;
  h.payload_len = 4 + sizeof body;
  h.type = MsgType::Delta;
  h.layout = Layout::Cellwise;
  h.codec = Codec::Lz4;
  h.seq = 0;
  write_frame_header(bad.data(), h);
  store_u32(bad.data() + kFrameHeaderSize, 100);  // claimed raw length
  std::memcpy(bad.data() + kFrameHeaderSize + 4, body, sizeof body);
  threw = false;
  try {
    sub3.on_bytes(bad.data(), bad.size());
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  section_done("stream hygiene");
}

int main() {
  test_varint();
  test_packing();
  test_rowset();
  test_lz4_golden();
  test_lz4_malformed();
  test_lz4_roundtrip();
  test_lz4_fuzz();
  test_deflate();
  test_end_to_end();
  test_seq_gap();
  test_stream_hygiene();
  std::printf("ALL PASS: %d checks\n", g_total);
  return 0;
}
