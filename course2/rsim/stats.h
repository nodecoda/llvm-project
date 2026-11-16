//===-- stats.h - per-mnemonic instruction histogram, chapter 10 --*- C++ -*-===//
//
// The chapter's "performance model" is one cycle per retired instruction.  This
// header only supplies the counts: the CPU bumps one bucket per step, and the
// driver prints them sorted so that the text is stable across runs.
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_STATS_H
#define RSIM_STATS_H

#include "decode.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace rsim {

class Stats {
public:
  void add(Insn id) { ++counts_[static_cast<unsigned>(id)]; }

  uint64_t count(Insn id) const { return counts_[static_cast<unsigned>(id)]; }

  uint64_t total() const {
    uint64_t sum = 0;
    for (uint64_t c : counts_)
      sum += c;
    return sum;
  }

  /// `instructions <total>`, then `<mnemonic> <count>` lines: largest count
  /// first, ties in mnemonic order.  Zero buckets are omitted.
  void print(std::FILE *out) const {
    std::vector<std::pair<uint64_t, const char *>> rows;
    rows.reserve(kInsnCount);
    for (unsigned i = 0; i < kInsnCount; ++i) {
      if (counts_[i] == 0)
        continue;
      rows.emplace_back(counts_[i], mnemonic(static_cast<Insn>(i)));
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
      if (a.first != b.first)
        return a.first > b.first;
      return std::strcmp(a.second, b.second) < 0;
    });
    std::fprintf(out, "instructions %" PRIu64 "\n", total());
    for (const auto &row : rows)
      std::fprintf(out, "%s %" PRIu64 "\n", row.second, row.first);
  }

private:
  uint64_t counts_[kInsnCount] = {};
};

} // namespace rsim

#endif // RSIM_STATS_H
