#include "colstream/subscriber.hpp"

#include <cstring>
#include <stdexcept>

#include "colstream/deflate.hpp"
#include "colstream/lz4.hpp"
#include "colstream/pack.hpp"

namespace colstream {

Feed::Feed(Schema schema, uint32_t nrows) : table_(std::move(schema), nrows) {}

void Feed::bad(const char* what) { throw std::runtime_error(std::string("colstream: ") + what); }

void Feed::on_bytes(const uint8_t* data, size_t len) {
  pending_.insert(pending_.end(), data, data + len);

  // Consume complete frames; keep the partial tail buffered.
  for (;;) {
    const size_t avail = pending_.size() - pending_off_;
    if (avail < kFrameHeaderSize) break;
    FrameHeader h;
    if (!read_frame_header(pending_.data() + pending_off_, h)) bad("bad frame header");
    if (avail < kFrameHeaderSize + h.payload_len) break;
    handle_frame(h, pending_.data() + pending_off_ + kFrameHeaderSize);
    pending_off_ += kFrameHeaderSize + h.payload_len;
  }
  if (pending_off_ == pending_.size()) {
    pending_.clear();
    pending_off_ = 0;
  } else if (pending_off_ > (64u << 10)) {  // compact occasionally
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(pending_off_));
    pending_off_ = 0;
  }
}

void Feed::handle_frame(const FrameHeader& h, const uint8_t* payload) {
  if (have_seq_ && h.seq != last_seq_ + 1) {
    gap_flag_ = true;
    gaps_++;
  }
  last_seq_ = h.seq;
  have_seq_ = true;

  if (h.codec == Codec::None) {
    apply_payload(h, payload, h.payload_len);
  } else {
    if (h.payload_len < 4) bad("short compressed payload");
    const uint32_t raw_len = load_u32(payload);
    if (raw_len > kMaxPayload) bad("bad raw length");
    raw_.resize(raw_len);
    long n;
    if (h.codec == Codec::Lz4)
      n = lz4_decompress(payload + 4, h.payload_len - 4, raw_.data(), raw_.size());
    else
      n = deflate_decompress(payload + 4, h.payload_len - 4, raw_.data(), raw_.size());
    if (n < 0 || static_cast<uint32_t>(n) != raw_len) bad("decompression failed");
    apply_payload(h, raw_.data(), raw_len);
  }
  frames_++;
}

void Feed::apply_payload(const FrameHeader& h, const uint8_t* p, size_t n) {
  if (h.type == MsgType::Snapshot) table_.reset();
  if (h.layout == Layout::Cellwise)
    apply_cellwise(p, n);
  else
    apply_columnar(p, n, h.layout == Layout::ColPacked);
}

void Feed::apply_cell(uint32_t row, uint16_t col, bool is_null, uint64_t bits) {
  if (col >= table_.ncols()) bad("column out of range");
  if (row >= table_.nrows()) bad("row out of range");
  if (is_null) {
    if (!table_.nullable(col)) bad("null cell in non-nullable column");
    table_.set_null(row, col);
  } else {
    table_.set_raw(row, col, bits);
  }
}

void Feed::apply_cellwise(const uint8_t* p, size_t n) {
  if (n % 14 != 0) bad("cellwise payload not a multiple of 14");
  for (const uint8_t* end = p + n; p < end; p += 14) {
    const uint32_t row = load_u32(p);
    const uint16_t colf = load_u16(p + 4);
    apply_cell(row, static_cast<uint16_t>(colf & 0x7FFF), (colf & 0x8000) != 0, load_u64(p + 6));
  }
}

void Feed::apply_columnar(const uint8_t* p, size_t n, bool packed) {
  const uint8_t* end = p + n;
  if (n < 2) bad("truncated columnar payload");
  const uint16_t nblocks = load_u16(p);
  p += 2;

  for (uint16_t b = 0; b < nblocks; b++) {
    if (end - p < 2) bad("truncated column block");
    const uint16_t col = load_u16(p);
    p += 2;
    if (col >= table_.ncols()) bad("column out of range");

    row_scratch_.clear();
    p = rowset_decode(p, end, table_.nrows(), row_scratch_);
    if (!p) bad("bad row-set");
    const size_t k = row_scratch_.size();

    // Null bitmask over the row-set, nullable columns only.
    const uint8_t* nulls = nullptr;
    size_t nvals = k;
    if (table_.nullable(col)) {
      const size_t nb = (k + 7) / 8;
      if (static_cast<size_t>(end - p) < nb) bad("truncated null bitmask");
      nulls = p;
      p += nb;
      nvals = 0;
      for (size_t i = 0; i < k; i++)
        if ((nulls[i / 8] & (1u << (i & 7))) == 0) nvals++;
    }

    if (!packed) {
      if (static_cast<size_t>(end - p) < nvals * 8) bad("truncated raw values");
      size_t vi = 0;
      for (size_t i = 0; i < k; i++) {
        if (nulls && (nulls[i / 8] & (1u << (i & 7)))) {
          table_.set_null(row_scratch_[i], col);
        } else {
          table_.set_raw(row_scratch_[i], col, load_u64(p + 8 * vi));
          vi++;
        }
      }
      p += nvals * 8;
    } else {
      key_scratch_.resize(nvals);
      p = unpack_values(p, end, nvals, key_scratch_.data());
      if (!p) bad("bad packed values");
      const bool is_i64 = table_.schema().col(col).type == Type::I64;
      size_t vi = 0;
      for (size_t i = 0; i < k; i++) {
        if (nulls && (nulls[i / 8] & (1u << (i & 7)))) {
          table_.set_null(row_scratch_[i], col);
        } else {
          const uint64_t key = key_scratch_[vi++];
          const uint64_t bits =
              is_i64 ? static_cast<uint64_t>(zigzag_dec(key)) : f64_unkey(key);
          table_.set_raw(row_scratch_[i], col, bits);
        }
      }
    }
  }
  if (p != end) bad("trailing bytes in columnar payload");
}

}  // namespace colstream
