// colstream: wire framing.
//
// Frame = [u32 payload_len][u8 msg_type][u8 layout][u8 codec][u64 seq][payload]
// All integers little-endian. payload_len counts payload bytes only.
// For codec != NONE the payload is [u32 raw_len][compressed bytes].
#pragma once

#include <cstddef>
#include <cstdint>

namespace colstream {

enum class MsgType : uint8_t { Snapshot = 0, Delta = 1 };
enum class Layout : uint8_t { Cellwise = 0, ColRaw = 1, ColPacked = 2 };
enum class Codec : uint8_t { None = 0, Lz4 = 1, Deflate = 2 };

constexpr size_t kFrameHeaderSize = 15;
// Sanity cap on payload_len; anything larger is treated as stream corruption.
constexpr uint32_t kMaxPayload = 1u << 30;

struct FrameHeader {
  uint32_t payload_len = 0;
  MsgType type = MsgType::Snapshot;
  Layout layout = Layout::Cellwise;
  Codec codec = Codec::None;
  uint64_t seq = 0;
};

inline void write_frame_header(uint8_t* dst, const FrameHeader& h) {
  for (int i = 0; i < 4; i++) dst[i] = static_cast<uint8_t>(h.payload_len >> (8 * i));
  dst[4] = static_cast<uint8_t>(h.type);
  dst[5] = static_cast<uint8_t>(h.layout);
  dst[6] = static_cast<uint8_t>(h.codec);
  for (int i = 0; i < 8; i++) dst[7 + i] = static_cast<uint8_t>(h.seq >> (8 * i));
}

// src must hold at least kFrameHeaderSize bytes. Returns false when the
// header fields are out of range.
inline bool read_frame_header(const uint8_t* src, FrameHeader& h) {
  h.payload_len = src[0] | (uint32_t{src[1]} << 8) | (uint32_t{src[2]} << 16) |
                  (uint32_t{src[3]} << 24);
  if (h.payload_len > kMaxPayload) return false;
  if (src[4] > 1 || src[5] > 2 || src[6] > 2) return false;
  h.type = static_cast<MsgType>(src[4]);
  h.layout = static_cast<Layout>(src[5]);
  h.codec = static_cast<Codec>(src[6]);
  uint64_t s = 0;
  for (int i = 0; i < 8; i++) s |= uint64_t{src[7 + i]} << (8 * i);
  h.seq = s;
  return true;
}

}  // namespace colstream
