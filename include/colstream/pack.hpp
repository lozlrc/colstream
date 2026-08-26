// colstream: value packing primitives and row-set encoding.
//
// Packed value scheme (COL_PACKED):
//   i64 -> zigzag -> u64 key; f64 -> byteswap(bit pattern) -> u64 key.
//   Each key is written as its minimal little-endian byte count (0..8).
//   Lengths are 4-bit nibbles, two per byte (even index low nibble, odd index
//   high nibble, zero-padded), all nibbles first, then the value bytes.
//
// Row-set: 1-byte selector, then either
//   0: raw bitset over nrows, LSB-first, ceil(nrows/8) bytes
//   1: varint count, varint first row, varint gaps (row[i] - row[i-1])
// The encoder picks whichever is smaller (bitset on ties).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

namespace colstream {

inline uint64_t zigzag_enc(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
inline int64_t zigzag_dec(uint64_t z) {
  return static_cast<int64_t>(z >> 1) ^ -static_cast<int64_t>(z & 1);
}

inline uint64_t bswap64(uint64_t x) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap64(x);
#else
  x = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFull);
  x = ((x & 0x0000FFFF0000FFFFull) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFull);
  return (x << 32) | (x >> 32);
#endif
}

// f64 wire key: byteswap so price-like doubles (zero mantissa low bytes)
// become keys with high zero bytes, which the minimal-byte scheme strips.
inline uint64_t f64_key(uint64_t bits) { return bswap64(bits); }
inline uint64_t f64_unkey(uint64_t key) { return bswap64(key); }

// Minimal bytes needed for k in little-endian (0 for k == 0).
inline unsigned min_bytes(uint64_t k) {
  return k == 0 ? 0u : static_cast<unsigned>((64 - std::countl_zero(k) + 7) / 8);
}

// Little-endian scalar append/read helpers.
inline void put_u16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
inline void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void put_u64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline uint16_t load_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (uint16_t{p[1]} << 8));
}
inline uint32_t load_u32(const uint8_t* p) {
  return p[0] | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
inline uint64_t load_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= uint64_t{p[i]} << (8 * i);
  return v;
}
inline void store_u32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void store_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// LEB128 varint.
inline unsigned varint_len(uint64_t v) {
  unsigned n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}
void put_varint(std::vector<uint8_t>& out, uint64_t v);
// Returns pointer past the varint, or nullptr on truncation/overlong input.
const uint8_t* get_varint(const uint8_t* p, const uint8_t* end, uint64_t& v);

// rows: ascending, unique, each < nrows. Appends the encoded row-set.
void rowset_encode(std::vector<uint8_t>& out, const uint32_t* rows, size_t n, uint32_t nrows);
// Appends decoded rows (ascending). Returns pointer past the row-set, or
// nullptr on malformed input.
const uint8_t* rowset_decode(const uint8_t* p, const uint8_t* end, uint32_t nrows,
                             std::vector<uint32_t>& rows);

// Packed values: appends nibble length array then value bytes for n keys.
void pack_values(std::vector<uint8_t>& out, const uint64_t* keys, size_t n);
// Reads n keys. Returns pointer past the packed block, or nullptr on
// malformed input (length nibble > 8, truncation).
const uint8_t* unpack_values(const uint8_t* p, const uint8_t* end, size_t n, uint64_t* keys);

}  // namespace colstream
