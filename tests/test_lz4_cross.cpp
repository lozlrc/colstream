// Cross-verification of the colstream LZ4 codec against reference liblz4.
// Built only when /opt/homebrew/include/lz4.h is present (see Makefile).
// Both directions: our compressor -> LZ4_decompress_safe, and
// LZ4_compress_default -> our decompressor. 500 seeded cases.
#include <lz4.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "colstream/lz4.hpp"

static int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::abort();                                                        \
    }                                                                      \
    g_checks++;                                                            \
  } while (0)

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

static std::vector<uint8_t> make_input(Rng& rng) {
  const uint32_t pick = rng.below(4);
  const size_t n = pick == 0 ? 1 + rng.below(64) : pick == 1 ? rng.below(2048) : rng.below(32768);
  std::vector<uint8_t> in(std::max<size_t>(n, 1));
  const uint32_t mode = rng.below(4);
  if (mode == 0) {
    for (auto& b : in) b = static_cast<uint8_t>(rng.next());
  } else if (mode == 1) {
    for (auto& b : in) b = static_cast<uint8_t>(rng.below(5));
  } else if (mode == 2) {
    size_t i = 0;
    while (i < in.size()) {
      const uint8_t v = static_cast<uint8_t>(rng.next());
      size_t run = 1 + rng.below(200);
      while (run-- && i < in.size()) in[i++] = v;
    }
  } else {
    uint64_t px = 1000000;
    for (size_t i = 0; i < in.size(); i++) {
      if (i % 8 == 0) px += rng.below(7) - 3;
      in[i] = static_cast<uint8_t>(px >> (8 * (i % 8)));
    }
  }
  return in;
}

int main() {
  Rng rng(0x0C505511);
  for (int iter = 0; iter < 500; iter++) {
    const std::vector<uint8_t> in = make_input(rng);
    const int n = static_cast<int>(in.size());

    // Direction 1: our compressor, reference decompressor.
    {
      std::vector<uint8_t> comp(colstream::lz4_compress_bound(in.size()));
      const size_t c = colstream::lz4_compress(in.data(), in.size(), comp.data(), comp.size());
      CHECK(c >= 1);
      std::vector<uint8_t> out(in.size());
      const int d = LZ4_decompress_safe(reinterpret_cast<const char*>(comp.data()),
                                        reinterpret_cast<char*>(out.data()),
                                        static_cast<int>(c), n);
      CHECK(d == n);
      CHECK(std::memcmp(out.data(), in.data(), in.size()) == 0);
    }

    // Direction 2: reference compressor, our decompressor.
    {
      std::vector<uint8_t> comp(static_cast<size_t>(LZ4_compressBound(n)));
      const int c = LZ4_compress_default(reinterpret_cast<const char*>(in.data()),
                                         reinterpret_cast<char*>(comp.data()), n,
                                         static_cast<int>(comp.size()));
      CHECK(c > 0);
      std::vector<uint8_t> out(in.size());
      const long d = colstream::lz4_decompress(comp.data(), static_cast<size_t>(c), out.data(),
                                               out.size());
      CHECK(d == static_cast<long>(in.size()));
      CHECK(std::memcmp(out.data(), in.data(), in.size()) == 0);
    }
  }
  std::printf("PASS lz4 cross-verification vs liblz4 %s: %d checks over 500 cases\n",
              LZ4_versionString(), g_checks);
  return 0;
}
