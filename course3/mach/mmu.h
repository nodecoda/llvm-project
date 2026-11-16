//===-- mmu.h - the Sv32 two-level walk, chapter 06 --------------*- C++ -*-===//
//
// Sv32 is small enough to read in one sitting: a 4 KiB root table, 1024
// second-level tables, and 4 KiB leaves.  M-mode never translates; the
// machine calls this walker only for U-mode fetches and data accesses.
//
// The walker is a pure function over physical memory so --selftest can hand it
// a table built by hand and check every permission bit without running a CPU.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_MMU_H
#define RMACH_MMU_H

#include "memory.h"
#include "trap.h"

#include <cstdint>
#include <vector>

namespace rmach {

enum class AccessType { Fetch, Load, Store };

class Mmu {
public:
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint32_t kPageMask = kPageSize - 1;

  // Sv32 PTE bits.
  static constexpr uint32_t kPteV = 1u << 0;
  static constexpr uint32_t kPteR = 1u << 1;
  static constexpr uint32_t kPteW = 1u << 2;
  static constexpr uint32_t kPteX = 1u << 3;
  static constexpr uint32_t kPteU = 1u << 4;
  static constexpr uint32_t kPteG = 1u << 5;
  static constexpr uint32_t kPteA = 1u << 6;
  static constexpr uint32_t kPteD = 1u << 7;
  static constexpr uint32_t kPteFlags = 0xffu;
  static constexpr uint32_t kPtePpnMask = 0xfffffc00u;

  /// Translate one U-mode address.  Throws Trap with cause 12/13/15 on a page
  /// fault.  M-mode must bypass this function instead of calling it.
  static uint32_t translate(const rsim::Memory &ram, uint32_t satp,
                            uint32_t va, AccessType type, uint32_t epc);

  /// One non-zero leaf, used by --dump-pagetable.  level 1 means a 4 MiB
  /// superpage, which translate() deliberately rejects in this course.
  struct Leaf {
    uint32_t va;
    uint32_t pte;
    uint32_t pa;
    int level;
  };
  static std::vector<Leaf> walk(const rsim::Memory &ram, uint32_t satp);
};

} // namespace rmach

#endif // RMACH_MMU_H
