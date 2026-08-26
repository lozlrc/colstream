#include "colstream/publisher.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

#include "colstream/deflate.hpp"
#include "colstream/lz4.hpp"
#include "colstream/pack.hpp"

namespace colstream {

Publisher::Publisher(Schema schema, uint32_t nrows) : table_(std::move(schema), nrows) {
  const size_t nc = table_.ncols();
  dirty_rows_.resize(nc);
  dirty_bits_.assign(nc, std::vector<uint64_t>((size_t{nrows} + 63) / 64, 0));
  all_rows_.resize(nrows);
  std::iota(all_rows_.begin(), all_rows_.end(), 0u);
}

void Publisher::check(uint32_t row, uint16_t col, Type t) const {
  if (col >= table_.ncols()) throw std::out_of_range("colstream: column out of range");
  if (row >= table_.nrows()) throw std::out_of_range("colstream: row out of range");
  if (table_.schema().col(col).type != t) throw std::logic_error("colstream: column type mismatch");
}

void Publisher::mark_dirty(uint32_t row, uint16_t col) {
  uint64_t& w = dirty_bits_[col][row >> 6];
  const uint64_t bit = uint64_t{1} << (row & 63);
  if ((w & bit) == 0) {
    w |= bit;
    dirty_rows_[col].push_back(row);
  }
}

void Publisher::set_i64(uint32_t row, uint16_t col, int64_t v) {
  check(row, col, Type::I64);
  table_.set_i64(row, col, v);
  mark_dirty(row, col);
}

void Publisher::set_f64(uint32_t row, uint16_t col, double v) {
  check(row, col, Type::F64);
  table_.set_f64(row, col, v);
  mark_dirty(row, col);
}

void Publisher::set_null(uint32_t row, uint16_t col) {
  if (col >= table_.ncols()) throw std::out_of_range("colstream: column out of range");
  if (row >= table_.nrows()) throw std::out_of_range("colstream: row out of range");
  if (!table_.nullable(col)) throw std::logic_error("colstream: column not nullable");
  table_.set_null(row, col);
  mark_dirty(row, col);
}

size_t Publisher::dirty_cells() const {
  size_t n = 0;
  for (const auto& v : dirty_rows_) n += v.size();
  return n;
}

void Publisher::clear_dirty() {
  for (size_t c = 0; c < dirty_rows_.size(); c++) {
    for (uint32_t r : dirty_rows_[c]) dirty_bits_[c][r >> 6] &= ~(uint64_t{1} << (r & 63));
    dirty_rows_[c].clear();
  }
}

// Rows to emit for a column: all rows for snapshots, sorted dirty rows for
// deltas. Delta rows are sorted in place (order does not matter elsewhere).
const std::vector<uint32_t>& Publisher::column_rows(uint16_t col, bool snapshot) {
  if (snapshot) return all_rows_;
  auto& rows = dirty_rows_[col];
  std::sort(rows.begin(), rows.end());
  return rows;
}

void Publisher::build_cellwise(bool snapshot) {
  const size_t nc = table_.ncols();
  for (uint16_t c = 0; c < nc; c++) {
    const auto& rows = column_rows(c, snapshot);
    const bool nullable = table_.nullable(c);
    for (uint32_t r : rows) {
      put_u32(raw_, r);
      const bool isnull = nullable && table_.is_null(r, c);
      put_u16(raw_, static_cast<uint16_t>(c | (isnull ? 0x8000u : 0u)));
      put_u64(raw_, table_.raw(r, c));  // zero when null
    }
  }
}

void Publisher::build_columnar(bool snapshot, bool packed) {
  const size_t nc = table_.ncols();
  uint16_t nblocks = 0;
  for (uint16_t c = 0; c < nc; c++)
    if (snapshot || !dirty_rows_[c].empty()) nblocks++;
  put_u16(raw_, nblocks);

  for (uint16_t c = 0; c < nc; c++) {
    const auto& rows = column_rows(c, snapshot);
    if (rows.empty()) continue;
    put_u16(raw_, c);
    rowset_encode(raw_, rows.data(), rows.size(), table_.nrows());

    const bool nullable = table_.nullable(c);
    row_scratch_.clear();  // non-null rows, in row-set order
    if (nullable) {
      const size_t base = raw_.size();
      raw_.resize(base + (rows.size() + 7) / 8, 0);
      for (size_t i = 0; i < rows.size(); i++) {
        if (table_.is_null(rows[i], c))
          raw_[base + i / 8] |= static_cast<uint8_t>(1u << (i & 7));
        else
          row_scratch_.push_back(rows[i]);
      }
    }
    const std::vector<uint32_t>& vrows = nullable ? row_scratch_ : rows;

    if (!packed) {
      for (uint32_t r : vrows) put_u64(raw_, table_.raw(r, c));
    } else {
      key_scratch_.clear();
      if (table_.schema().col(c).type == Type::I64) {
        for (uint32_t r : vrows)
          key_scratch_.push_back(zigzag_enc(static_cast<int64_t>(table_.raw(r, c))));
      } else {
        for (uint32_t r : vrows) key_scratch_.push_back(f64_key(table_.raw(r, c)));
      }
      pack_values(raw_, key_scratch_.data(), key_scratch_.size());
    }
  }
}

std::span<const uint8_t> Publisher::finish(MsgType type, Layout layout, Codec codec) {
  FrameHeader h;
  h.type = type;
  h.layout = layout;
  h.codec = codec;
  h.seq = seq_;

  size_t payload_len;
  if (codec == Codec::None) {
    out_.resize(kFrameHeaderSize + raw_.size());
    std::copy(raw_.begin(), raw_.end(), out_.begin() + kFrameHeaderSize);
    payload_len = raw_.size();
  } else {
    // Compressed payload: [u32 raw_len][compressed bytes].
    const size_t bound =
        (codec == Codec::Lz4) ? lz4_compress_bound(raw_.size()) : deflate_bound(raw_.size());
    out_.resize(kFrameHeaderSize + 4 + bound);
    store_u32(out_.data() + kFrameHeaderSize, static_cast<uint32_t>(raw_.size()));
    const size_t csize =
        (codec == Codec::Lz4)
            ? lz4_compress(raw_.data(), raw_.size(), out_.data() + kFrameHeaderSize + 4, bound)
            : deflate_compress(raw_.data(), raw_.size(), out_.data() + kFrameHeaderSize + 4, bound);
    if (csize == 0 && raw_.size() != 0) throw std::runtime_error("colstream: compression failed");
    payload_len = 4 + csize;
  }
  if (payload_len > kMaxPayload) throw std::runtime_error("colstream: payload too large");
  h.payload_len = static_cast<uint32_t>(payload_len);
  out_.resize(kFrameHeaderSize + payload_len);
  write_frame_header(out_.data(), h);

  seq_++;
  clear_dirty();
  return {out_.data(), out_.size()};
}

std::span<const uint8_t> Publisher::publish_snapshot(Layout layout, Codec codec) {
  raw_.clear();
  if (layout == Layout::Cellwise)
    build_cellwise(true);
  else
    build_columnar(true, layout == Layout::ColPacked);
  return finish(MsgType::Snapshot, layout, codec);
}

std::span<const uint8_t> Publisher::publish_delta(Layout layout, Codec codec) {
  raw_.clear();
  if (layout == Layout::Cellwise)
    build_cellwise(false);
  else
    build_columnar(false, layout == Layout::ColPacked);
  return finish(MsgType::Delta, layout, codec);
}

}  // namespace colstream
