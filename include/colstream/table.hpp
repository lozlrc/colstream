// colstream: column-major table of i64/f64 cells with optional per-column
// validity bitmaps. Values are stored as raw 64-bit patterns, so f64 NaN
// payloads and signed zeros survive round trips bit-exact.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "schema.hpp"

namespace colstream {

class Table {
 public:
  Table(Schema schema, uint32_t nrows);

  const Schema& schema() const { return schema_; }
  uint32_t nrows() const { return nrows_; }
  size_t ncols() const { return schema_.ncols(); }
  bool nullable(uint16_t col) const { return !valid_[col].empty(); }

  // Unchecked hot-path accessors. Callers validate row/col/type.
  void set_i64(uint32_t row, uint16_t col, int64_t v) { store(row, col, static_cast<uint64_t>(v)); }
  void set_f64(uint32_t row, uint16_t col, double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    store(row, col, b);
  }
  // Stores the raw bit pattern and marks the cell valid.
  void set_raw(uint32_t row, uint16_t col, uint64_t bits) { store(row, col, bits); }
  // Clears the cell: value bits go to zero so tables compare deterministically.
  void set_null(uint32_t row, uint16_t col) {
    data_[col][row] = 0;
    valid_[col][row >> 6] &= ~(uint64_t{1} << (row & 63));
  }

  int64_t get_i64(uint32_t row, uint16_t col) const { return static_cast<int64_t>(data_[col][row]); }
  double get_f64(uint32_t row, uint16_t col) const {
    double d;
    std::memcpy(&d, &data_[col][row], 8);
    return d;
  }
  uint64_t raw(uint32_t row, uint16_t col) const { return data_[col][row]; }
  bool is_null(uint32_t row, uint16_t col) const {
    const auto& v = valid_[col];
    if (v.empty()) return false;
    return (v[row >> 6] & (uint64_t{1} << (row & 63))) == 0;
  }

  // Values back to zero; nullable columns all-null, others all-valid.
  void reset();
  // Bit-exact comparison of values and validity (NaN payloads included).
  bool equals(const Table& other) const;

 private:
  void store(uint32_t row, uint16_t col, uint64_t bits) {
    data_[col][row] = bits;
    auto& v = valid_[col];
    if (!v.empty()) v[row >> 6] |= uint64_t{1} << (row & 63);
  }

  Schema schema_;
  uint32_t nrows_ = 0;
  std::vector<std::vector<uint64_t>> data_;   // [col][row] raw 64-bit values
  std::vector<std::vector<uint64_t>> valid_;  // [col] bitmap words, empty if not nullable
};

}  // namespace colstream
