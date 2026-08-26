#include "colstream/table.hpp"

namespace colstream {

Table::Table(Schema schema, uint32_t nrows) : schema_(std::move(schema)), nrows_(nrows) {
  const size_t nc = schema_.ncols();
  data_.resize(nc);
  valid_.resize(nc);
  const size_t words = (size_t{nrows_} + 63) / 64;
  for (size_t c = 0; c < nc; c++) {
    data_[c].assign(nrows_, 0);
    if (schema_.col(c).nullable) valid_[c].assign(words, 0);  // starts all-null
  }
}

void Table::reset() {
  for (size_t c = 0; c < data_.size(); c++) {
    std::fill(data_[c].begin(), data_[c].end(), 0);
    std::fill(valid_[c].begin(), valid_[c].end(), 0);
  }
}

bool Table::equals(const Table& other) const {
  if (nrows_ != other.nrows_ || data_.size() != other.data_.size()) return false;
  for (size_t c = 0; c < data_.size(); c++) {
    if (data_[c] != other.data_[c]) return false;
    if (valid_[c] != other.valid_[c]) return false;
  }
  return true;
}

}  // namespace colstream
