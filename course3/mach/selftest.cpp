//===-- selftest.cpp - hand-computed CSR vectors, chapter 02 ----------------===//
//
// Every check has an expected value a reader can recompute from the spec:
// masks, read-only registers, alignment of mepc/mtvec, and the U-mode
// privilege check.
//
//===----------------------------------------------------------------------===//

#include "selftest.h"

#include "csr.h"
#include "machine.h"

#include <cstdio>

namespace rmach {
namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

template <typename F>
void expectTrap(F fn, TrapCause expected, const char *what) {
  ++checks;
  try {
    fn();
    ++failures;
    std::fprintf(stderr, "FAIL: %s (no trap)\n", what);
  } catch (const Trap &trap) {
    if (trap.cause != expected) {
      ++failures;
      std::fprintf(stderr, "FAIL: %s (wrong cause %u)\n", what,
                   static_cast<unsigned>(trap.cause));
    }
  }
}

constexpr uint32_t kMmuRoot = 0x80010000;
constexpr uint32_t kMmuL1 = 0x80011000;
constexpr uint32_t kMmuLeaf = 0x80012000;
constexpr uint32_t kMmuSatp = 0x80000000u | (kMmuRoot >> 12);

/// A fresh root + second-level table, both mapped in the host memory model.
rsim::Memory makeMmuMemory() {
  rsim::Memory mem;
  mem.map(kMmuRoot, Mmu::kPageSize);
  mem.map(kMmuL1, Mmu::kPageSize);
  return mem;
}

/// Install one 4 KiB leaf for va.  The root points at kMmuL1; each test uses
/// a fresh memory so the same virtual address can be remapped with new flags.
void mapLeaf(rsim::Memory &mem, uint32_t va, uint32_t pa, uint32_t flags) {
  const uint32_t vpn1 = (va >> 22) & 0x3ff;
  const uint32_t vpn0 = (va >> 12) & 0x3ff;
  mem.write32(kMmuRoot + vpn1 * 4,
              ((kMmuL1 >> 12) << 10) | Mmu::kPteV);
  mem.write32(kMmuL1 + vpn0 * 4, ((pa >> 12) << 10) | flags);
}

} // namespace

int runSelfTest() {
  rsim::Image image;
  image.entry = 0x80000000;
  Bus bus;
  Machine m;
  m.reset(image, bus);

  // mstatus only keeps MIE/MPIE/MPP.
  m.writeCsr(kCsrMstatus, 0xffffffffu);
  check(m.readCsr(kCsrMstatus) == 0x1888u, "mstatus write mask");

  // misa: RV32 (MXL=1), I and M.  Writes are ignored.
  check(m.readCsr(kCsrMisa) == 0x40001100u, "misa reports RV32IM");
  m.writeCsr(kCsrMisa, 0);
  check(m.readCsr(kCsrMisa) == 0x40001100u, "misa writes are ignored");

  check(m.readCsr(kCsrMhartid) == 0, "mhartid is zero");
  check(m.readCsr(kCsrMvendorid) == 0, "mvendorid is zero");
  check(m.readCsr(kCsrMarchid) == 0, "marchid is zero");
  check(m.readCsr(kCsrMimpid) == 0, "mimpid is zero");

  // mtvec: base is 4-byte aligned, mode is 0 or 1.
  m.writeCsr(kCsrMtvec, 0x80000005u);
  check(m.readCsr(kCsrMtvec) == 0x80000005u, "mtvec keeps vectored mode");
  m.writeCsr(kCsrMtvec, 0x8000000au);
  check(m.readCsr(kCsrMtvec) == 0x80000008u, "mtvec rejects mode 2");

  m.writeCsr(kCsrMepc, 0x12345679u);
  check(m.readCsr(kCsrMepc) == 0x12345678u, "mepc clears the low two bits");

  m.writeCsr(kCsrMscratch, 0xdeadbeefu);
  check(m.readCsr(kCsrMscratch) == 0xdeadbeefu, "mscratch round-trips");

  m.writeCsr(kCsrMcause, 0x80000007u);
  check(m.readCsr(kCsrMcause) == 0x80000007u, "mcause keeps the interrupt bit");
  m.writeCsr(kCsrMtval, 0xcafebabeu);
  check(m.readCsr(kCsrMtval) == 0xcafebabeu, "mtval round-trips");

  m.writeCsr(kCsrMie, 0xffffffffu);
  check(m.readCsr(kCsrMie) == 0x80u, "mie only keeps MTIE");
  m.writeCsr(kCsrMip, 0xffffffffu);
  check(m.readCsr(kCsrMip) == 0, "mip is owned by the CLINT");

  m.writeCsr(kCsrMinstret, 123u);
  check(m.readCsr(kCsrMinstret) == 0, "minstret is read-only");

  m.writeCsr(kCsrSatp, 0xffffffffu);
  check(m.readCsr(kCsrSatp) == 0x803fffffu, "satp keeps MODE/ASID/PPN");

  expectTrap([&] { m.readCsr(0x123); }, TrapCause::IllegalInstruction,
             "unknown CSR read");
  expectTrap([&] { m.writeCsr(kCsrMhartid, 1); },
             TrapCause::IllegalInstruction, "read-only CSR write");

  m.setPrivilege(Priv::U);
  expectTrap([&] { m.readCsr(kCsrMstatus); },
             TrapCause::IllegalInstruction, "U-mode mstatus read");
  expectTrap([&] { m.writeCsr(kCsrSatp, 0); },
             TrapCause::IllegalInstruction, "U-mode satp write");
  m.setPrivilege(Priv::M);
  check(m.readCsr(kCsrMstatus) == 0x1888u, "M-mode mstatus read again");

  // Chapter 06: the Sv32 walker, one permission bit per check.  These are
  // unit checks over hand-built tables; tests/mmu-run.c exercises the same
  // walker through a real U-mode fetch/load/store later in the gate.
  const uint32_t all = Mmu::kPteV | Mmu::kPteR | Mmu::kPteW | Mmu::kPteX |
                       Mmu::kPteU | Mmu::kPteA | Mmu::kPteD;
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf, all);
    check(Mmu::translate(mem, kMmuSatp, 0x80000123, AccessType::Load, 0) ==
              0x80012123,
          "Sv32 load translation");
    check(Mmu::translate(mem, kMmuSatp, 0x80000123, AccessType::Fetch, 0) ==
              0x80012123,
          "Sv32 fetch translation");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf, all & ~Mmu::kPteU);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Load, 0); },
        TrapCause::LoadPageFault, "Sv32 U=0 denies a U load");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf, all & ~Mmu::kPteX);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Fetch, 0); },
        TrapCause::InstructionPageFault, "Sv32 non-executable fetch");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf, all & ~Mmu::kPteW);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Store, 0); },
        TrapCause::StorePageFault, "Sv32 read-only store");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf, all & ~Mmu::kPteA);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Load, 0); },
        TrapCause::LoadPageFault, "Sv32 A=0 load");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf, all & ~Mmu::kPteD);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Store, 0); },
        TrapCause::StorePageFault, "Sv32 D=0 store");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mapLeaf(mem, 0x80000000, kMmuLeaf,
            Mmu::kPteV | Mmu::kPteW | Mmu::kPteU | Mmu::kPteA | Mmu::kPteD);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Load, 0); },
        TrapCause::LoadPageFault, "Sv32 W=1 with R=0 is reserved");
  }
  {
    rsim::Memory mem = makeMmuMemory(); // all root entries are invalid
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Load, 0); },
        TrapCause::LoadPageFault, "Sv32 invalid root PTE");
  }
  {
    rsim::Memory mem = makeMmuMemory();
    mem.write32(kMmuRoot + ((0x80000000u >> 22) * 4),
                ((0x80000000u >> 12) << 10) | all);
    expectTrap(
        [&] { Mmu::translate(mem, kMmuSatp, 0x80000000, AccessType::Load, 0); },
        TrapCause::LoadPageFault, "Sv32 superpage is rejected");
  }

  std::printf("selftest: %d checks, %d failures\n", checks, failures);
  return failures;
}

} // namespace rmach
