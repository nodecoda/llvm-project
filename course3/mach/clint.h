//===-- clint.h - mtime/mtimecmp, chapter 04 ---------------------*- C++ -*-===//
//
// The machine timer is the first device: two 64-bit registers in MMIO space.
// mtime ticks once per retired instruction (the course's cycle model), and
// mtime >= mtimecmp raises MTIP.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_CLINT_H
#define RMACH_CLINT_H

#include <cstdint>

namespace rmach {

class Clint {
public:
  static constexpr uint32_t kBase = 0x02000000;
  static constexpr uint32_t kMtimecmp = kBase + 0x4000;
  static constexpr uint32_t kMtime = kBase + 0xbff8;

  void reset() {
    mtime_ = 0;
    mtimecmp_ = ~uint64_t{0};
  }

  void tick() { ++mtime_; }
  bool pending() const { return mtime_ >= mtimecmp_; }

  uint64_t mtime() const { return mtime_; }
  uint64_t mtimecmp() const { return mtimecmp_; }

  void writeMtimecmp(uint32_t value, bool high) {
    if (high)
      mtimecmp_ = (mtimecmp_ & 0xffffffffull) | (uint64_t{value} << 32);
    else
      mtimecmp_ = (mtimecmp_ & ~0xffffffffull) | value;
  }

  bool contains(uint32_t addr) const { return addr - kBase < 0x10000u; }

private:
  uint64_t mtime_ = 0;
  uint64_t mtimecmp_ = ~uint64_t{0};
};

} // namespace rmach

#endif // RMACH_CLINT_H
