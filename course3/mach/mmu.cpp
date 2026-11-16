//===-- mmu.cpp - walk, permissions and page faults, chapter 06 -------------===//

#include "mmu.h"

namespace rmach {

namespace {

TrapCause causeFor(AccessType type) {
  switch (type) {
  case AccessType::Fetch:
    return TrapCause::InstructionPageFault;
  case AccessType::Load:
    return TrapCause::LoadPageFault;
  case AccessType::Store:
    return TrapCause::StorePageFault;
  }
  return TrapCause::LoadPageFault;
}

[[noreturn]] void pageFault(AccessType type, uint32_t va, uint32_t epc,
                            const char *why) {
  throw Trap(causeFor(type), va, epc, why);
}

uint32_t readPte(const rsim::Memory &ram, uint32_t addr, AccessType type,
                 uint32_t va, uint32_t epc) {
  try {
    return ram.read32(addr);
  } catch (const rsim::MemoryError &) {
    pageFault(type, va, epc, "page-table page is unmapped");
  }
}

/// A 4 KiB leaf keeps A/D under software control: the course never sets them
/// behind the kernel's back, so A=0 or D=0 is observable and testable.
void checkLeaf(uint32_t pte, AccessType type, uint32_t va, uint32_t epc) {
  const bool r = pte & Mmu::kPteR;
  const bool w = pte & Mmu::kPteW;
  const bool x = pte & Mmu::kPteX;

  if (!r && !w && !x)
    pageFault(type, va, epc, "non-leaf PTE at level 0");
  if (w && !r)
    pageFault(type, va, epc, "W=1 with R=0 is reserved");
  if (!(pte & Mmu::kPteU))
    pageFault(type, va, epc, "U=0 page accessed from U-mode");
  if (type == AccessType::Fetch && !x)
    pageFault(type, va, epc, "page is not executable");
  if (type == AccessType::Load && !r)
    pageFault(type, va, epc, "page is not readable");
  if (type == AccessType::Store && !w)
    pageFault(type, va, epc, "page is not writable");
  if (!(pte & Mmu::kPteA))
    pageFault(type, va, epc, "A=0; hardware does not set it in this course");
  if (type == AccessType::Store && !(pte & Mmu::kPteD))
    pageFault(type, va, epc, "D=0 on a store");
}

} // namespace

uint32_t Mmu::translate(const rsim::Memory &ram, uint32_t satp, uint32_t va,
                        AccessType type, uint32_t epc) {
  if (!(satp & 0x80000000u))
    pageFault(type, va, epc, "satp.MODE is Bare");

  const uint32_t root = (satp & 0x003fffffu) << 12;
  const uint32_t vpn1 = (va >> 22) & 0x3ff;
  const uint32_t pte1 = readPte(ram, root + vpn1 * 4, type, va, epc);

  if (!(pte1 & kPteV))
    pageFault(type, va, epc, "invalid root PTE");

  // A leaf at level 1 is a 4 MiB superpage.  Chapters 06-07 only build 4 KiB
  // leaves; rejecting it keeps the PA calculation honest instead of silently
  // taking the wrong bits.  Supporting it is an exercise.
  if (pte1 & (kPteR | kPteW | kPteX))
    pageFault(type, va, epc, "4 MiB superpage is not supported");

  const uint32_t level1 = (pte1 & kPtePpnMask) << 2;
  const uint32_t vpn0 = (va >> 12) & 0x3ff;
  const uint32_t pte0 = readPte(ram, level1 + vpn0 * 4, type, va, epc);

  if (!(pte0 & kPteV))
    pageFault(type, va, epc, "invalid level-0 PTE");

  checkLeaf(pte0, type, va, epc);
  return ((pte0 & kPtePpnMask) << 2) | (va & kPageMask);
}

std::vector<Mmu::Leaf> Mmu::walk(const rsim::Memory &ram, uint32_t satp) {
  std::vector<Leaf> leaves;
  const uint32_t root = (satp & 0x003fffffu) << 12;

  for (uint32_t i = 0; i < 1024; ++i) {
    uint32_t pte1 = 0;
    try {
      pte1 = ram.read32(root + i * 4);
    } catch (const rsim::MemoryError &) {
      break; // the root table itself is not mapped
    }
    if (!(pte1 & kPteV))
      continue;

    if (pte1 & (kPteR | kPteW | kPteX)) {
      leaves.push_back({i << 22, pte1, (pte1 & kPtePpnMask) << 2, 1});
      continue;
    }

    const uint32_t level1 = (pte1 & kPtePpnMask) << 2;
    for (uint32_t j = 0; j < 1024; ++j) {
      uint32_t pte0 = 0;
      try {
        pte0 = ram.read32(level1 + j * 4);
      } catch (const rsim::MemoryError &) {
        break; // the second-level table is not mapped
      }
      if (!(pte0 & kPteV))
        continue;
      leaves.push_back(
          {(i << 22) | (j << 12), pte0, (pte0 & kPtePpnMask) << 2, 0});
    }
  }
  return leaves;
}

} // namespace rmach
