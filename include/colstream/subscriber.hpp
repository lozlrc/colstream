// colstream: subscriber. Accepts an arbitrarily fragmented byte stream,
// reassembles frames, decompresses, and applies them to a local table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "frame.hpp"
#include "table.hpp"

namespace colstream {

class Feed {
 public:
  Feed(Schema schema, uint32_t nrows);

  // Consume any number of bytes at any fragmentation. Partial frames are
  // buffered until complete. Throws std::runtime_error on corrupt frames.
  void on_bytes(const uint8_t* data, size_t len);

  const Table& table() const { return table_; }
  size_t frames_applied() const { return frames_; }
  uint64_t last_seq() const { return last_seq_; }

  // Sequence-gap tracking: flagged when a frame arrives whose seq is not
  // last_seq + 1. The frame is still applied; the caller decides recovery.
  bool seq_gap() const { return gap_flag_; }
  uint64_t gap_count() const { return gaps_; }
  void clear_gap_flag() { gap_flag_ = false; }

 private:
  void handle_frame(const FrameHeader& h, const uint8_t* payload);
  void apply_payload(const FrameHeader& h, const uint8_t* p, size_t n);
  void apply_cellwise(const uint8_t* p, size_t n);
  void apply_columnar(const uint8_t* p, size_t n, bool packed);
  void apply_cell(uint32_t row, uint16_t col, bool is_null, uint64_t bits);
  [[noreturn]] static void bad(const char* what);

  Table table_;
  std::vector<uint8_t> pending_;  // partial frame accumulation
  size_t pending_off_ = 0;
  std::vector<uint8_t> raw_;  // decompressed payload
  std::vector<uint32_t> row_scratch_;
  std::vector<uint64_t> key_scratch_;
  size_t frames_ = 0;
  uint64_t last_seq_ = 0;
  bool have_seq_ = false;
  bool gap_flag_ = false;
  uint64_t gaps_ = 0;
};

}  // namespace colstream
