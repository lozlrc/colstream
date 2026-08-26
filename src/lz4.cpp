// LZ4 block format codec.
//
// Implemented from the public block format description:
//   sequence = token (high nibble literal length, low nibble match length
//   minus 4), optional 255-extension bytes after a nibble of 15, literal
//   bytes, 2-byte little-endian offset in 1..65535, optional match length
//   extension bytes. The block ends with a literals-only sequence.
//
// Encoder constraints honored: minimum match 4, the last 5 bytes of input
// are always literals, and no match starts within the last 12 bytes.
// The compressor is a single-probe 4-byte-hash table over a 64 KiB window
// with the step-skipping acceleration of the fast reference path.
#include "colstream/lz4.hpp"

#include <cstring>
#include <vector>

namespace colstream {
namespace {

constexpr unsigned kHashLog = 12;  // 4096-entry table, 16 KiB
constexpr size_t kHashSize = size_t{1} << kHashLog;
constexpr size_t kMinMatch = 4;
constexpr size_t kMfLimit = 12;      // no match may start in the last 12 bytes
constexpr size_t kLastLiterals = 5;  // last 5 bytes are literals
constexpr size_t kMaxDistance = 65535;
constexpr unsigned kSkipTrigger = 6;  // step = misses >> kSkipTrigger

inline uint32_t read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline uint32_t hash4(uint32_t x) { return (x * 2654435761u) >> (32 - kHashLog); }

// Emits a literal-length or match-length value into token nibble + extensions.
inline void put_len(uint8_t*& op, uint8_t* token, unsigned shift, size_t len) {
  if (len >= 15) {
    *token = static_cast<uint8_t>(*token | (15u << shift));
    len -= 15;
    while (len >= 255) {
      *op++ = 255;
      len -= 255;
    }
    *op++ = static_cast<uint8_t>(len);
  } else {
    *token = static_cast<uint8_t>(*token | (len << shift));
  }
}

}  // namespace

size_t lz4_compress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap) {
  if (dst_cap < lz4_compress_bound(src_len)) return 0;
  uint8_t* op = dst;
  if (src_len == 0) {  // literals-only empty sequence
    *op++ = 0;
    return 1;
  }

  const uint8_t* ip = src;
  const uint8_t* anchor = src;
  const uint8_t* const iend = src + src_len;

  if (src_len >= kMfLimit + 1) {
    const uint8_t* const mflimit = iend - kMfLimit;         // match starts must be < mflimit
    const uint8_t* const matchlimit = iend - kLastLiterals;  // matches may extend to here

    static thread_local std::vector<uint32_t> table;
    table.assign(kHashSize, 0);  // entries store position + 1; 0 means empty

    table[hash4(read32(ip))] = 1;
    ip++;

    for (;;) {
      const uint8_t* match;

      // Find a match: single hash probe per position, step grows with misses.
      {
        const uint8_t* fwd = ip;
        size_t step = 1;
        unsigned misses = 1u << kSkipTrigger;
        for (;;) {
          ip = fwd;
          fwd = ip + step;
          step = misses++ >> kSkipTrigger;
          if (ip >= mflimit) goto last_literals;
          const uint32_t h = hash4(read32(ip));
          const uint32_t c = table[h];
          table[h] = static_cast<uint32_t>(ip - src) + 1;
          if (c != 0) {
            const uint8_t* cand = src + (c - 1);
            if (static_cast<size_t>(ip - cand) <= kMaxDistance && read32(cand) == read32(ip)) {
              match = cand;
              break;
            }
          }
        }
      }

      // Extend the match backwards over pending literals.
      while (ip > anchor && match > src && ip[-1] == match[-1]) {
        --ip;
        --match;
      }

      size_t lit = static_cast<size_t>(ip - anchor);

      // Emit sequences; the inner loop chains zero-literal sequences when the
      // position right after a match hits again.
      for (;;) {
        uint8_t* const token = op++;
        *token = 0;
        put_len(op, token, 4, lit);
        std::memcpy(op, anchor, lit);
        op += lit;

        const size_t off = static_cast<size_t>(ip - match);
        *op++ = static_cast<uint8_t>(off);
        *op++ = static_cast<uint8_t>(off >> 8);

        const uint8_t* p = ip + kMinMatch;
        const uint8_t* m = match + kMinMatch;
        while (p < matchlimit && *p == *m) {
          ++p;
          ++m;
        }
        put_len(op, token, 0, static_cast<size_t>(p - ip) - kMinMatch);
        ip = p;
        anchor = ip;

        if (ip >= mflimit) goto last_literals;

        // Re-seed the table inside the span we just skipped, then test the
        // position immediately after the match.
        table[hash4(read32(ip - 2))] = static_cast<uint32_t>(ip - 2 - src) + 1;
        const uint32_t h = hash4(read32(ip));
        const uint32_t c = table[h];
        table[h] = static_cast<uint32_t>(ip - src) + 1;
        if (c != 0) {
          const uint8_t* cand = src + (c - 1);
          if (static_cast<size_t>(ip - cand) <= kMaxDistance && read32(cand) == read32(ip)) {
            match = cand;
            lit = 0;
            continue;
          }
        }
        ip++;  // already probed ip; resume searching one past it
        break;
      }
    }
  }

last_literals:
  const size_t lit = static_cast<size_t>(iend - anchor);
  uint8_t* const token = op++;
  *token = 0;
  put_len(op, token, 4, lit);
  std::memcpy(op, anchor, lit);
  op += lit;
  return static_cast<size_t>(op - dst);
}

long lz4_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap) {
  const uint8_t* ip = src;
  const uint8_t* const iend = src + src_len;
  uint8_t* op = dst;
  uint8_t* const oend = dst + dst_cap;
  if (src_len == 0) return -1;

  for (;;) {
    if (ip >= iend) return -1;  // truncated: token expected
    const unsigned token = *ip++;

    // Literal run.
    size_t lit = token >> 4;
    if (lit == 15) {
      for (;;) {
        if (ip >= iend) return -1;  // truncated extension
        const unsigned b = *ip++;
        lit += b;
        if (b != 255) break;
      }
    }
    if (static_cast<size_t>(iend - ip) < lit) return -1;  // truncated literals
    if (static_cast<size_t>(oend - op) < lit) return -1;  // output overflow
    std::memcpy(op, ip, lit);
    op += lit;
    ip += lit;
    if (ip == iend) return static_cast<long>(op - dst);  // literals-only final sequence

    // Match.
    if (static_cast<size_t>(iend - ip) < 2) return -1;  // truncated offset
    const size_t off = ip[0] | (size_t{ip[1]} << 8);
    ip += 2;
    if (off == 0) return -1;                              // invalid offset
    if (off > static_cast<size_t>(op - dst)) return -1;   // offset beyond window start
    size_t mlen = (token & 15) + kMinMatch;
    if ((token & 15) == 15) {
      for (;;) {
        if (ip >= iend) return -1;  // truncated extension
        const unsigned b = *ip++;
        mlen += b;
        if (b != 255) break;
      }
    }
    if (static_cast<size_t>(oend - op) < mlen) return -1;  // output overflow
    const uint8_t* m = op - off;
    if (off >= mlen) {
      std::memcpy(op, m, mlen);
    } else {
      for (size_t i = 0; i < mlen; i++) op[i] = m[i];  // overlapping copy
    }
    op += mlen;
  }
}

}  // namespace colstream
