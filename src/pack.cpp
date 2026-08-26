#include "colstream/pack.hpp"

namespace colstream {

void put_varint(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

const uint8_t* get_varint(const uint8_t* p, const uint8_t* end, uint64_t& v) {
  uint64_t acc = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t b = *p++;
    if (shift == 63 && (b & 0x7E)) return nullptr;  // would overflow u64
    acc |= uint64_t{b & 0x7Fu} << shift;
    if ((b & 0x80) == 0) {
      v = acc;
      return p;
    }
    shift += 7;
    if (shift > 63) return nullptr;
  }
  return nullptr;  // truncated
}

void rowset_encode(std::vector<uint8_t>& out, const uint32_t* rows, size_t n, uint32_t nrows) {
  const size_t bitset_size = 1 + (size_t{nrows} + 7) / 8;
  size_t list_size = 1 + varint_len(n);
  uint32_t prev = 0;
  for (size_t i = 0; i < n; i++) {
    list_size += varint_len(i == 0 ? rows[i] : rows[i] - prev);
    prev = rows[i];
    if (list_size >= bitset_size) break;  // bitset already won
  }
  if (list_size < bitset_size) {
    out.push_back(1);
    put_varint(out, n);
    prev = 0;
    for (size_t i = 0; i < n; i++) {
      put_varint(out, i == 0 ? rows[i] : rows[i] - prev);
      prev = rows[i];
    }
  } else {
    out.push_back(0);
    const size_t base = out.size();
    out.resize(base + (size_t{nrows} + 7) / 8, 0);
    for (size_t i = 0; i < n; i++) out[base + (rows[i] >> 3)] |= uint8_t(1u << (rows[i] & 7));
  }
}

const uint8_t* rowset_decode(const uint8_t* p, const uint8_t* end, uint32_t nrows,
                             std::vector<uint32_t>& rows) {
  if (p >= end) return nullptr;
  const uint8_t sel = *p++;
  if (sel == 0) {
    const size_t nb = (size_t{nrows} + 7) / 8;
    if (static_cast<size_t>(end - p) < nb) return nullptr;
    for (size_t byte = 0; byte < nb; byte++) {
      uint8_t b = p[byte];
      while (b) {
        const unsigned bit = static_cast<unsigned>(std::countr_zero(b));
        const uint32_t row = static_cast<uint32_t>(byte * 8 + bit);
        if (row >= nrows) return nullptr;  // stray bit past nrows
        rows.push_back(row);
        b = static_cast<uint8_t>(b & (b - 1));
      }
    }
    return p + nb;
  }
  if (sel == 1) {
    uint64_t n;
    p = get_varint(p, end, n);
    if (!p || n > nrows) return nullptr;
    uint64_t prev = 0;
    for (uint64_t i = 0; i < n; i++) {
      uint64_t d;
      p = get_varint(p, end, d);
      if (!p) return nullptr;
      const uint64_t row = (i == 0) ? d : prev + d;
      if (row >= nrows || (i > 0 && d == 0)) return nullptr;  // out of range / not ascending
      rows.push_back(static_cast<uint32_t>(row));
      prev = row;
    }
    return p;
  }
  return nullptr;  // unknown selector
}

void pack_values(std::vector<uint8_t>& out, const uint64_t* keys, size_t n) {
  const size_t nib_base = out.size();
  out.resize(nib_base + (n + 1) / 2, 0);
  for (size_t i = 0; i < n; i++) {
    const uint64_t k = keys[i];
    const unsigned len = min_bytes(k);
    uint8_t& nb = out[nib_base + i / 2];
    nb = static_cast<uint8_t>(nb | ((i & 1) ? (len << 4) : len));
    for (unsigned b = 0; b < len; b++) out.push_back(static_cast<uint8_t>(k >> (8 * b)));
  }
}

const uint8_t* unpack_values(const uint8_t* p, const uint8_t* end, size_t n, uint64_t* keys) {
  const size_t nib_bytes = (n + 1) / 2;
  if (static_cast<size_t>(end - p) < nib_bytes) return nullptr;
  const uint8_t* nib = p;
  p += nib_bytes;
  for (size_t i = 0; i < n; i++) {
    const unsigned len = (i & 1) ? (nib[i / 2] >> 4) : (nib[i / 2] & 0xF);
    if (len > 8) return nullptr;
    if (static_cast<size_t>(end - p) < len) return nullptr;
    uint64_t k = 0;
    for (unsigned b = 0; b < len; b++) k |= uint64_t{p[b]} << (8 * b);
    keys[i] = k;
    p += len;
  }
  return p;
}

}  // namespace colstream
