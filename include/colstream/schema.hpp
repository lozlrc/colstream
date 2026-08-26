// colstream: runtime column schema.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace colstream {

enum class Type : uint8_t { I64 = 0, F64 = 1 };

struct ColumnDef {
  std::string name;
  Type type = Type::I64;
  bool nullable = false;
};

// Immutable column list. Column ids are u16; the top bit is reserved on the
// wire (CELLWISE null flag), so at most 0x8000 columns are allowed.
class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<ColumnDef> cols) : cols_(std::move(cols)) {
    if (cols_.size() > 0x8000) throw std::invalid_argument("schema: too many columns");
  }

  size_t ncols() const { return cols_.size(); }
  const ColumnDef& col(size_t i) const { return cols_.at(i); }
  const std::vector<ColumnDef>& cols() const { return cols_; }

 private:
  std::vector<ColumnDef> cols_;
};

}  // namespace colstream
