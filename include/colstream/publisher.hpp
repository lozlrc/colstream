// colstream: publisher. Owns the authoritative table, tracks per-column
// dirty rows as writes happen, and builds snapshot/delta frames into
// reusable internal buffers.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "frame.hpp"
#include "table.hpp"

namespace colstream {

class Publisher {
 public:
  Publisher(Schema schema, uint32_t nrows);

  const Table& table() const { return table_; }
  uint32_t nrows() const { return table_.nrows(); }
  uint64_t next_seq() const { return seq_; }

  // Checked writes: validate row/col/type and mark the cell dirty.
  void set_i64(uint32_t row, uint16_t col, int64_t v);
  void set_f64(uint32_t row, uint16_t col, double v);
  void set_null(uint32_t row, uint16_t col);

  // Build one frame (header + payload) and return a view of it. The span is
  // valid until the next publish_* call. Both calls consume one sequence
  // number and clear the dirty set (a snapshot carries all state).
  std::span<const uint8_t> publish_snapshot(Layout layout, Codec codec);
  std::span<const uint8_t> publish_delta(Layout layout, Codec codec);

  // Total dirty cells currently pending.
  size_t dirty_cells() const;

 private:
  void check(uint32_t row, uint16_t col, Type t) const;
  void mark_dirty(uint32_t row, uint16_t col);
  void clear_dirty();
  const std::vector<uint32_t>& column_rows(uint16_t col, bool snapshot);
  void build_cellwise(bool snapshot);
  void build_columnar(bool snapshot, bool packed);
  std::span<const uint8_t> finish(MsgType type, Layout layout, Codec codec);

  Table table_;
  std::vector<std::vector<uint32_t>> dirty_rows_;  // per column, unsorted
  std::vector<std::vector<uint64_t>> dirty_bits_;  // per column dedup bitmap
  std::vector<uint32_t> all_rows_;                 // 0..nrows-1, for snapshots
  std::vector<uint32_t> row_scratch_;
  std::vector<uint64_t> key_scratch_;
  std::vector<uint8_t> raw_;  // payload build buffer
  std::vector<uint8_t> out_;  // frame buffer (header + possibly compressed payload)
  uint64_t seq_ = 0;
};

}  // namespace colstream
