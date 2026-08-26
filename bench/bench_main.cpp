// colstream benchmark: synthetic market table, zipf-distributed row updates,
// every layout x codec combination. Steady-state per-message cost measured
// with clock_gettime after a warmup, identical update stream per combo.
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "colstream/publisher.hpp"
#include "colstream/subscriber.hpp"

using namespace colstream;

namespace {

constexpr uint32_t kRows = 5000;
constexpr int kUpdatesPerDelta = 200;
constexpr int kDeltas = 5000;
constexpr int kWarmup = 500;
constexpr uint64_t kSeed = 0xBE7C4A5E;

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
};

uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Columns: 4 f64 prices with 2 to 4 decimal places, 8 i64 sizes and counters.
Schema bench_schema() {
  return Schema({
      {"bid", Type::F64, false},      // 0
      {"ask", Type::F64, false},      // 1
      {"last", Type::F64, false},     // 2
      {"vwap", Type::F64, false},     // 3
      {"bid_sz", Type::I64, false},   // 4
      {"ask_sz", Type::I64, false},   // 5
      {"last_sz", Type::I64, false},  // 6
      {"volume", Type::I64, false},   // 7
      {"trade_ct", Type::I64, false}, // 8
      {"ts_ns", Type::I64, false},    // 9
      {"seq", Type::I64, false},      // 10
      {"flags", Type::I64, false},    // 11
  });
}

// Market update generator. Zipf(1.2) over instrument ranks: hot symbols
// update often, so per-column dirty row sets stay concentrated.
struct Gen {
  Rng rng;
  std::vector<double> cdf;
  std::vector<uint32_t> perm;
  std::vector<double> tick;
  std::vector<int64_t> mid_ticks;
  int64_t ts_ns = 1724232000000000000ll;
  int64_t seqno = 0;

  Gen() : rng(kSeed) {
    cdf.resize(kRows);
    double sum = 0;
    for (uint32_t r = 0; r < kRows; r++) sum += 1.0 / std::pow(static_cast<double>(r + 1), 1.2);
    double acc = 0;
    for (uint32_t r = 0; r < kRows; r++) {
      acc += 1.0 / std::pow(static_cast<double>(r + 1), 1.2) / sum;
      cdf[r] = acc;
    }
    perm.resize(kRows);
    for (uint32_t r = 0; r < kRows; r++) perm[r] = r;
    for (uint32_t r = kRows - 1; r > 0; r--) std::swap(perm[r], perm[rng.below(r + 1)]);
    tick.resize(kRows);
    mid_ticks.resize(kRows);
    for (uint32_t r = 0; r < kRows; r++) {
      tick[r] = (r % 3 == 0) ? 0.01 : (r % 3 == 1) ? 0.0025 : 0.0001;  // 2 to 4 dp
      mid_ticks[r] = 1000 + static_cast<int64_t>(rng.below(4000000));
    }
  }

  uint32_t sample_row() {
    const double u = rng.uniform();
    const auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
    return perm[static_cast<uint32_t>(it - cdf.begin())];
  }

  void init_table(Publisher& pub) {
    for (uint32_t r = 0; r < kRows; r++) {
      const double t = tick[r];
      pub.set_f64(r, 0, static_cast<double>(mid_ticks[r] - 1) * t);
      pub.set_f64(r, 1, static_cast<double>(mid_ticks[r] + 1) * t);
      pub.set_f64(r, 2, static_cast<double>(mid_ticks[r]) * t);
      pub.set_f64(r, 3, static_cast<double>(mid_ticks[r]) * t);
      pub.set_i64(r, 4, 100 * (1 + rng.below(50)));
      pub.set_i64(r, 5, 100 * (1 + rng.below(50)));
      pub.set_i64(r, 6, 0);
      pub.set_i64(r, 7, 0);
      pub.set_i64(r, 8, 0);
      pub.set_i64(r, 9, ts_ns);
      pub.set_i64(r, 10, 0);
      pub.set_i64(r, 11, 0);
    }
  }

  void update(Publisher& pub) {
    const uint32_t r = sample_row();
    const double t = tick[r];
    mid_ticks[r] += static_cast<int64_t>(rng.below(5)) - 2;
    ts_ns += 200 + rng.below(1000);
    pub.set_f64(r, 0, static_cast<double>(mid_ticks[r] - 1) * t);
    pub.set_f64(r, 1, static_cast<double>(mid_ticks[r] + 1) * t);
    pub.set_i64(r, 4, 100 * (1 + rng.below(50)));
    pub.set_i64(r, 5, 100 * (1 + rng.below(50)));
    pub.set_i64(r, 9, ts_ns);
    pub.set_i64(r, 10, seqno++);
    if (rng.below(100) < 30) {  // trade
      const int64_t sz = 100 * (1 + rng.below(20));
      pub.set_f64(r, 2, static_cast<double>(mid_ticks[r]) * t);
      pub.set_i64(r, 6, sz);
      pub.set_i64(r, 7, pub.table().get_i64(r, 7) + sz);
      pub.set_i64(r, 8, pub.table().get_i64(r, 8) + 1);
    }
    if (rng.below(64) == 0) {
      pub.set_f64(r, 3, static_cast<double>(mid_ticks[r]) * t);
      pub.set_i64(r, 11, static_cast<int64_t>(rng.below(8)));
    }
  }
};

struct Result {
  double payload_bytes;
  double pub_ns;
  double sub_ns;
  size_t snapshot_payload;
};

const char* layout_name(Layout l) {
  switch (l) {
    case Layout::Cellwise: return "CELLWISE";
    case Layout::ColRaw: return "COL_RAW";
    default: return "COL_PACKED";
  }
}
const char* codec_name(Codec c) {
  switch (c) {
    case Codec::None: return "NONE";
    case Codec::Lz4: return "LZ4";
    default: return "DEFLATE";
  }
}

Result run_combo(Layout layout, Codec codec) {
  Publisher pub(bench_schema(), kRows);
  Feed sub(bench_schema(), kRows);
  Gen gen;  // fixed seed: identical update stream for every combo
  gen.init_table(pub);
  {
    auto f = pub.publish_snapshot(layout, codec);
    sub.on_bytes(f.data(), f.size());
  }

  uint64_t pub_ns = 0, sub_ns = 0, bytes = 0;
  int measured = 0;
  for (int msg = 0; msg < kDeltas; msg++) {
    for (int u = 0; u < kUpdatesPerDelta; u++) gen.update(pub);
    const uint64_t t0 = now_ns();
    auto f = pub.publish_delta(layout, codec);
    const uint64_t t1 = now_ns();
    sub.on_bytes(f.data(), f.size());
    const uint64_t t2 = now_ns();
    if (msg >= kWarmup) {
      pub_ns += t1 - t0;
      sub_ns += t2 - t1;
      bytes += f.size() - kFrameHeaderSize;
      measured++;
    }
  }
  if (!sub.table().equals(pub.table())) {
    std::fprintf(stderr, "FAIL: subscriber diverged (%s %s)\n", layout_name(layout),
                 codec_name(codec));
    std::abort();
  }

  Result r;
  r.payload_bytes = static_cast<double>(bytes) / measured;
  r.pub_ns = static_cast<double>(pub_ns) / measured;
  r.sub_ns = static_cast<double>(sub_ns) / measured;
  auto snap = pub.publish_snapshot(layout, codec);
  r.snapshot_payload = snap.size() - kFrameHeaderSize;
  sub.on_bytes(snap.data(), snap.size());
  if (!sub.table().equals(pub.table())) {
    std::fprintf(stderr, "FAIL: snapshot diverged\n");
    std::abort();
  }
  return r;
}

}  // namespace

int main() {
  std::printf("colstream bench\n");
  std::printf("Apple M2 Pro, macOS, clang 17, -O2\n");
  std::printf("table %u rows x 12 cols, zipf(1.2) row updates, %d updates/delta, "
              "%d deltas/combo, warmup %d\n\n",
              kRows, kUpdatesPerDelta, kDeltas, kWarmup);

  const Layout layouts[] = {Layout::Cellwise, Layout::ColRaw, Layout::ColPacked};
  const Codec codecs[] = {Codec::None, Codec::Lz4, Codec::Deflate};

  Result res[3][3];
  for (int l = 0; l < 3; l++)
    for (int c = 0; c < 3; c++) res[l][c] = run_combo(layouts[l], codecs[c]);

  const double base = res[0][0].payload_bytes;
  std::printf("| layout     | codec   | payload B/msg | ratio | pub ns/msg | sub ns/msg |\n");
  std::printf("|------------|---------|--------------:|------:|-----------:|-----------:|\n");
  for (int l = 0; l < 3; l++) {
    for (int c = 0; c < 3; c++) {
      const Result& r = res[l][c];
      std::printf("| %-10s | %-7s | %13.1f | %5.3f | %10.0f | %10.0f |\n",
                  layout_name(layouts[l]), codec_name(codecs[c]), r.payload_bytes,
                  r.payload_bytes / base, r.pub_ns, r.sub_ns);
    }
  }

  std::printf("\nsnapshot payload bytes (%u rows x 12 cols):\n", kRows);
  std::printf("| layout     |    NONE |     LZ4 | DEFLATE |\n");
  std::printf("|------------|--------:|--------:|--------:|\n");
  for (int l = 0; l < 3; l++) {
    std::printf("| %-10s | %7zu | %7zu | %7zu |\n", layout_name(layouts[l]),
                res[l][0].snapshot_payload, res[l][1].snapshot_payload,
                res[l][2].snapshot_payload);
  }
  return 0;
}
