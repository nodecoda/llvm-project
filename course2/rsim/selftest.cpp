//===-- selftest.cpp - hand-computed instruction vectors, chapter 03 ------===//
//
// The external gate compares 97 real encodings with llvm-mc, but that only
// proves the decoder agrees with the assembler.  These tests prove the
// *executor*: one hand-computed answer per instruction family, plus the
// corners that are easy to get wrong (x0, shift masks, sltiu -1, div edges).
//
// Instructions are built with tiny encoders below instead of a hex table, so
// every test reads like assembly.
//
//===----------------------------------------------------------------------===//

#include "selftest.h"

#include "cpu.h"
#include "decode.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>

namespace rsim {
namespace {

constexpr uint32_t kCode = 0x1000;
constexpr uint32_t kData = 0x2000;
constexpr unsigned kA = 5; // t0
constexpr unsigned kB = 6; // t1
constexpr unsigned kD = 7; // t2

uint32_t encR(uint8_t f3, uint8_t f7, unsigned rd, unsigned rs1, unsigned rs2) {
  return static_cast<uint32_t>(f7) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
         rd << 7 | 0x33;
}

uint32_t encI(uint8_t opcode, uint8_t f3, unsigned rd, unsigned rs1,
              int32_t imm) {
  return (static_cast<uint32_t>(imm) & 0xfff) << 20 | rs1 << 15 | f3 << 12 |
         rd << 7 | opcode;
}

uint32_t encS(uint8_t f3, unsigned rs1, unsigned rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xfff;
  return (u >> 5) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u & 0x1f) << 7 |
         0x23;
}

uint32_t encB(uint8_t f3, unsigned rs1, unsigned rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0x1fff;
  return ((u >> 12) & 1) << 31 | ((u >> 5) & 0x3f) << 25 | rs2 << 20 |
         rs1 << 15 | f3 << 12 | ((u >> 1) & 0xf) << 8 | ((u >> 11) & 1) << 7 |
         0x63;
}

uint32_t encU(uint8_t opcode, unsigned rd, uint32_t imm20) {
  return (imm20 & 0xfffff) << 12 | rd << 7 | opcode;
}

uint32_t encA(uint8_t f5, unsigned rd, unsigned rs1, unsigned rs2) {
  return static_cast<uint32_t>(f5) << 27 | rs2 << 20 | rs1 << 15 | 0x2 << 12 |
         rd << 7 | 0x2f;
}

uint32_t encJ(unsigned rd, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0x1fffff;
  return ((u >> 20) & 1) << 31 | ((u >> 1) & 0x3ff) << 21 |
         ((u >> 11) & 1) << 20 | ((u >> 12) & 0xff) << 12 | rd << 7 | 0x6f;
}

struct Fixture {
  Memory mem;
  Cpu cpu;

  Fixture() {
    mem.map(kCode, 0x1000);
    mem.map(kData, 0x1000);
    cpu.attach(mem);
    cpu.pc = kCode;
  }

  bool exec(uint32_t word) {
    cpu.pc = kCode; // every vector starts at the same address
    mem.write32(kCode, word);
    return cpu.step();
  }
};

int g_checks = 0;
int g_failures = 0;

void expect(const char *name, uint32_t got, uint32_t want) {
  ++g_checks;
  if (got == want)
    return;
  ++g_failures;
  std::printf("FAIL %-24s got 0x%08x want 0x%08x\n", name, got, want);
}

} // namespace

int runSelfTest() {
  // Register-register ALU (opcode 0x33).
  auto alu = [](const char *name, uint32_t word, uint32_t a, uint32_t b,
                uint32_t want) {
    Fixture f;
    f.cpu.x[kA] = a;
    f.cpu.x[kB] = b;
    f.exec(word);
    expect(name, f.cpu.x[kD], want);
  };
  // Register-immediate ALU and shifts.
  auto imm = [](const char *name, uint32_t word, uint32_t a, uint32_t want) {
    Fixture f;
    f.cpu.x[kA] = a;
    f.exec(word);
    expect(name, f.cpu.x[kD], want);
  };
  // Branches: the expected pc, not the registers, is the answer.
  auto branch = [](const char *name, uint32_t word, uint32_t a, uint32_t b,
                   uint32_t want_pc) {
    Fixture f;
    f.cpu.x[kA] = a;
    f.cpu.x[kB] = b;
    f.exec(word);
    expect(name, f.cpu.pc, want_pc);
  };

  alu("add", encR(0x0, 0x00, kD, kA, kB), 5, 6, 11);
  alu("sub", encR(0x0, 0x20, kD, kA, kB), 3, 5, static_cast<uint32_t>(-2));
  alu("sll masks the count", encR(0x1, 0x00, kD, kA, kB), 1, 33, 2);
  alu("slt", encR(0x2, 0x00, kD, kA, kB), 0xffffffffu, 1, 1);
  alu("sltu", encR(0x3, 0x00, kD, kA, kB), 0xffffffffu, 1, 0);
  alu("xor", encR(0x4, 0x00, kD, kA, kB), 0xf0, 0x0f, 0xff);
  alu("srl", encR(0x5, 0x00, kD, kA, kB), 0x80000000u, 31, 1);
  alu("sra", encR(0x5, 0x20, kD, kA, kB), 0x80000000u, 31, 0xffffffffu);
  alu("or", encR(0x6, 0x00, kD, kA, kB), 0xf0, 0x0f, 0xff);
  alu("and", encR(0x7, 0x00, kD, kA, kB), 0xff, 0x0f, 0x0f);

  imm("addi", encI(0x13, 0x0, kD, kA, -100), 100, 0);
  imm("addi -2048", encI(0x13, 0x0, kD, kA, -2048), 0, 0xfffff800u);
  imm("slti", encI(0x13, 0x2, kD, kA, 0), 0xffffffffu, 1);
  imm("sltiu -1", encI(0x13, 0x3, kD, kA, -1), 0, 1);
  imm("sltiu wraps", encI(0x13, 0x3, kD, kA, -1), 0xffffffffu, 0);
  imm("xori", encI(0x13, 0x4, kD, kA, -1), 0xff, 0xffffff00u);
  imm("ori", encI(0x13, 0x6, kD, kA, 0x0f), 0x100, 0x10f);
  imm("andi", encI(0x13, 0x7, kD, kA, 0x0f), 0xff, 0x0f);
  imm("slli 31", encI(0x13, 0x1, kD, kA, 31), 1, 0x80000000u);
  imm("srli 31", encI(0x13, 0x5, kD, kA, 31), 0x80000000u, 1);
  imm("srai 31", encI(0x13, 0x5, kD, kA, 0x400 | 31), 0x80000000u, 0xffffffffu);

  // M extension: the four divide-by-zero / overflow corners are specified.
  alu("mul low half", encR(0x0, 0x01, kD, kA, kB), 0x10000, 0x10000, 0);
  alu("mul -1 * -1", encR(0x0, 0x01, kD, kA, kB), 0xffffffffu, 0xffffffffu, 1);
  alu("mulh", encR(0x1, 0x01, kD, kA, kB), 0x80000000u, 0x80000000u,
      0x40000000u);
  alu("mulhsu", encR(0x2, 0x01, kD, kA, kB), 0xffffffffu, 0xffffffffu,
      0xffffffffu);
  alu("mulhu", encR(0x3, 0x01, kD, kA, kB), 0xffffffffu, 0xffffffffu,
      0xfffffffeu);
  alu("div 7/2", encR(0x4, 0x01, kD, kA, kB), 7, 2, 3);
  alu("div -7/2", encR(0x4, 0x01, kD, kA, kB), 0xfffffff9u, 2, 0xfffffffdu);
  alu("div by zero", encR(0x4, 0x01, kD, kA, kB), 1, 0, 0xffffffffu);
  alu("div overflow", encR(0x4, 0x01, kD, kA, kB), 0x80000000u, 0xffffffffu,
      0x80000000u);
  alu("divu", encR(0x5, 0x01, kD, kA, kB), 0xffffffffu, 2, 0x7fffffffu);
  alu("divu by zero", encR(0x5, 0x01, kD, kA, kB), 5, 0, 0xffffffffu);
  alu("rem -7%2", encR(0x6, 0x01, kD, kA, kB), 0xfffffff9u, 2, 0xffffffffu);
  alu("rem by zero", encR(0x6, 0x01, kD, kA, kB), 7, 0, 7);
  alu("rem overflow", encR(0x6, 0x01, kD, kA, kB), 0x80000000u, 0xffffffffu, 0);
  alu("remu", encR(0x7, 0x01, kD, kA, kB), 7, 2, 1);
  alu("remu by zero", encR(0x7, 0x01, kD, kA, kB), 7, 0, 7);

  // x0 is not a register: every write to it is dropped.
  {
    Fixture f;
    f.exec(encI(0x13, 0x0, 0, 0, 5));
    expect("x0 ignores addi", f.cpu.x[0], 0);
    f.cpu.x[kA] = 1;
    f.cpu.x[kB] = 2;
    f.exec(encR(0x0, 0x00, 0, kA, kB));
    expect("x0 ignores add", f.cpu.x[0], 0);
  }

  // Upper immediates and pc-relative arithmetic.
  imm("lui", encU(0x37, kD, 0x12345), 0, 0x12345000u);
  imm("auipc", encU(0x17, kD, 1), 0, kCode + 0x1000);

  // Jumps: the link value is the address of the *next* instruction.
  {
    Fixture f;
    f.exec(encJ(kD, 8));
    expect("jal link", f.cpu.x[kD], kCode + 4);
    expect("jal target", f.cpu.pc, kCode + 8);
  }
  {
    Fixture f;
    f.cpu.x[kA] = kData + 1; // odd on purpose: the LSB is cleared
    f.exec(encI(0x67, 0x0, kD, kA, 0));
    expect("jalr clears the LSB", f.cpu.pc, kData);
    expect("jalr link", f.cpu.x[kD], kCode + 4);
  }

  // Branches: six comparisons, taken and not taken.
  branch("beq taken", encB(0x0, kA, kB, 8), 1, 1, kCode + 8);
  branch("beq fallthrough", encB(0x0, kA, kB, 8), 1, 2, kCode + 4);
  branch("bne taken", encB(0x1, kA, kB, 8), 1, 2, kCode + 8);
  branch("blt taken", encB(0x4, kA, kB, 8), 0xffffffffu, 1, kCode + 8);
  branch("bge taken", encB(0x5, kA, kB, 8), 1, 0xffffffffu, kCode + 8);
  branch("bltu taken", encB(0x6, kA, kB, 8), 1, 0xffffffffu, kCode + 8);
  branch("bgeu taken", encB(0x7, kA, kB, 8), 0xffffffffu, 1, kCode + 8);
  branch("branch backwards", encB(0x0, kA, kB, -4), 0, 0, kCode - 4);

  // Loads and stores round-trip through the data page.
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.cpu.x[kB] = 0x11223344;
    f.exec(encS(0x2, kA, kB, 4));
    expect("sw", f.mem.read32(kData + 4), 0x11223344);
    f.exec(encI(0x03, 0x2, kD, kA, 4));
    expect("lw", f.cpu.x[kD], 0x11223344);
  }
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.cpu.x[kB] = 0x12345678;
    f.exec(encS(0x0, kA, kB, 0));
    expect("sb keeps one byte", f.mem.read32(kData), 0x78);
    f.exec(encI(0x03, 0x0, kD, kA, 0));
    expect("lb sign-extends", f.cpu.x[kD], 0x78);
    f.mem.write8(kData, 0x80);
    f.exec(encI(0x03, 0x0, kD, kA, 0));
    expect("lb of 0x80", f.cpu.x[kD], 0xffffff80u);
    f.exec(encI(0x03, 0x4, kD, kA, 0));
    expect("lbu of 0x80", f.cpu.x[kD], 0x80);
  }
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.cpu.x[kB] = 0xdeadbeef;
    f.exec(encS(0x1, kA, kB, 2));
    expect("sh", f.mem.read16(kData + 2), 0xbeef);
    f.exec(encI(0x03, 0x1, kD, kA, 2));
    expect("lh sign-extends", f.cpu.x[kD], 0xffffbeefu);
    f.exec(encI(0x03, 0x5, kD, kA, 2));
    expect("lhu", f.cpu.x[kD], 0xbeef);
  }

  // fence is a no-op on one hart; ecall stops the step loop.
  {
    Fixture f;
    f.exec(encI(0x0f, 0x0, 0, 0, 0));
    expect("fence advances pc", f.cpu.pc, kCode + 4);
  }
  {
    Fixture f;
    f.cpu.x[17] = 93; // exit(0)
    expect("ecall halts", f.exec(encI(0x73, 0x0, 0, 0, 0)) ? 1u : 0u, 0u);
    expect("ecall sets halted", f.cpu.halted() ? 1u : 0u, 1u);
    expect("ecall exit code", static_cast<uint32_t>(f.cpu.exitCode()), 0u);
  }

  // Chapter 04: precise traps.  epc must be the faulting instruction, not the
  // one after it, and tval must be the address that could not be used.
  auto trap = [](const char *name, uint32_t word, unsigned reg, uint32_t value,
                 TrapCause want_cause, uint32_t want_tval) {
    Fixture f;
    f.cpu.x[reg] = value;
    f.cpu.x[kB] = value; // branch vectors compare rs1 against rs2
    try {
      f.exec(word);
    } catch (const Trap &t) {
      ++g_checks;
      if (t.cause == want_cause && t.tval == want_tval && t.epc == kCode)
        return;
      ++g_failures;
      std::printf("FAIL %-24s got cause %u tval 0x%08x epc 0x%08x\n", name,
                  static_cast<unsigned>(t.cause), t.tval, t.epc);
      return;
    }
    ++g_checks;
    ++g_failures;
    std::printf("FAIL %-24s did not trap\n", name);
  };

  trap("jalr off a 4-byte boundary", encI(0x67, 0x0, kD, kA, 2), kA, kData,
       TrapCause::InstructionAddressMisaligned, kData + 2);
  trap("jal off a 4-byte boundary", encJ(kD, 2), kA, 0,
       TrapCause::InstructionAddressMisaligned, kCode + 2);
  trap("branch off a 4-byte boundary", encB(0x0, kA, kB, 2), kA, 1,
       TrapCause::InstructionAddressMisaligned, kCode + 2);
  trap("lw misaligned", encI(0x03, 0x2, kD, kA, 2), kA, kData,
       TrapCause::LoadAddressMisaligned, kData + 2);
  trap("lh misaligned", encI(0x03, 0x1, kD, kA, 1), kA, kData,
       TrapCause::LoadAddressMisaligned, kData + 1);
  trap("sw misaligned", encS(0x2, kA, kB, 3), kA, kData,
       TrapCause::StoreAddressMisaligned, kData + 3);
  trap("sh misaligned", encS(0x1, kA, kB, 1), kA, kData,
       TrapCause::StoreAddressMisaligned, kData + 1);
  trap("load from nowhere", encI(0x03, 0x0, kD, kA, 0), kA, 0x90000000u,
       TrapCause::LoadAccessFault, 0x90000000u);
  trap("store to nowhere", encS(0x2, kA, kB, 0), kA, 0x90000000u,
       TrapCause::StoreAccessFault, 0x90000000u);
  trap("illegal instruction", 0x00000000u, kA, 0,
       TrapCause::IllegalInstruction, 0x00000000u);
  {
    Fixture f;
    f.cpu.pc = kCode + 2;
    try {
      f.cpu.step();
      ++g_checks;
      ++g_failures;
      std::printf("FAIL %-24s did not trap\n", "fetch off a boundary");
    } catch (const Trap &t) {
      ++g_checks;
      if (t.cause != TrapCause::InstructionAddressMisaligned ||
          t.tval != kCode + 2 || t.epc != kCode + 2) {
        ++g_failures;
        std::printf("FAIL %-24s got cause %u tval 0x%08x epc 0x%08x\n",
                    "fetch off a boundary", static_cast<unsigned>(t.cause),
                    t.tval, t.epc);
      }
    }
  }

  // Chapter 08: the CSR file and the trap entry.
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.exec(encI(0x73, 0x1, kD, kA, 0x305)); // csrrw t2, mtvec, t0
    expect("csrrw writes mtvec", f.cpu.mtvec(), kData);
    expect("csrrw returns the old value", f.cpu.x[kD], 0);
  }
  {
    Fixture f;
    f.cpu.x[kA] = 5;
    f.exec(encI(0x73, 0x1, 0, kA, 0x342)); // mcause = 5
    expect("csrrw writes mcause", f.cpu.mcause(), 5);
    f.cpu.x[kA] = 2;
    f.exec(encI(0x73, 0x2, 0, kA, 0x342)); // csrrs sets bits
    expect("csrrs sets bits", f.cpu.mcause(), 7);
    f.cpu.x[kA] = 1;
    f.exec(encI(0x73, 0x3, 0, kA, 0x342)); // csrrc clears bits
    expect("csrrc clears bits", f.cpu.mcause(), 6);
    f.exec(encI(0x73, 0x6, 0, 1, 0x342));  // csrrsi zimm = 1
    expect("csrrsi", f.cpu.mcause(), 7);
    f.exec(encI(0x73, 0x5, kD, 7, 0x343)); // csrrwi zimm = 7
    expect("csrrwi", f.cpu.mtval(), 7);
  }
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.exec(encI(0x73, 0x1, 0, kA, 0x341)); // mepc = 0x2000
    f.exec(0x30200073u);                    // mret
    expect("mret returns to mepc", f.cpu.pc, kData);
  }
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.exec(encI(0x73, 0x1, 0, kA, 0x305)); // mtvec = handler
    expect("trap enters the handler",
           f.exec(0x00000000u) ? 1u : 0u, 1u);
    expect("trap jumps to mtvec", f.cpu.pc, kData);
    expect("trap mcause", f.cpu.mcause(), 2);
    expect("trap mepc", f.cpu.mepc(), kCode);
    expect("trap mtval", f.cpu.mtval(), 0);
  }
  {
    Fixture f;
    f.cpu.x[kA] = kData;
    f.exec(encI(0x73, 0x1, 0, kA, 0x305));
    f.exec(encI(0x73, 0x0, 0, 0, 1)); // ebreak
    expect("ebreak mcause", f.cpu.mcause(), 3);
    expect("ebreak mepc", f.cpu.mepc(), kCode);
    expect("ebreak mtval", f.cpu.mtval(), kCode);
  }
  trap("unknown CSR", encI(0x73, 0x1, 0, 0, 0xfff), kA, 0,
       TrapCause::IllegalInstruction, 0xfff);

  // Chapter 09: Zbb, Zbs and A, three vectors each, plus the ISA filter.
  {
    Fixture f;
    f.cpu.x[kA] = 0xff00;
    f.cpu.x[kB] = 0x0f0f;
    f.exec(encR(0x7, 0x20, kD, kA, kB)); // andn
    expect("andn", f.cpu.x[kD], 0xf000);
    f.cpu.x[kA] = 0x0000ffff;
    f.exec(encI(0x13, 0x1, kD, kA, 0x600)); // clz
    expect("clz", f.cpu.x[kD], 16);
    f.cpu.x[kA] = 0;
    f.exec(encI(0x13, 0x1, kD, kA, 0x600)); // clz of zero is 32, not undefined
    expect("clz 0", f.cpu.x[kD], 32);
    f.cpu.x[kA] = 0x80000001u;
    f.cpu.x[kB] = 1;
    f.exec(encR(0x1, 0x30, kD, kA, kB)); // rol
    expect("rol", f.cpu.x[kD], 3);
  }
  {
    Fixture f;
    f.cpu.x[kA] = 0xff00;
    f.cpu.x[kB] = 3;
    f.exec(encR(0x1, 0x14, kD, kA, kB)); // bset
    expect("bset", f.cpu.x[kD], 0xff08);
    f.cpu.x[kA] = 0x10;
    f.exec(encI(0x13, 0x5, kD, kA, 0x484)); // bexti, shamt = 4
    expect("bexti", f.cpu.x[kD], 1);
  }
  {
    Fixture f;
    f.mem.write32(kData, 0xdeadbeef);
    f.cpu.x[kA] = kData;
    f.exec(encA(0x02, kD, kA, 0)); // lr.w
    expect("lr.w", f.cpu.x[kD], 0xdeadbeef);
    f.cpu.x[kB] = 0x11223344;
    f.exec(encA(0x03, kD, kA, kB)); // sc.w
    expect("sc.w stores", f.mem.read32(kData), 0x11223344);
    expect("sc.w succeeds", f.cpu.x[kD], 0);
    f.mem.write32(kData, 5);
    f.cpu.x[kB] = 7;
    f.exec(encA(0x00, kD, kA, kB)); // amoadd.w
    expect("amoadd.w returns the old value", f.cpu.x[kD], 5);
    expect("amoadd.w writes the sum", f.mem.read32(kData), 12);
    f.mem.write32(kData, 5);
    f.exec(encA(0x1c, kD, kA, kB)); // amomaxu.w
    expect("amomaxu.w", f.mem.read32(kData), 7);
  }
  {
    Fixture f;
    f.cpu.setIsa(static_cast<uint32_t>(Isa::RV32I)); // no Zbb
    f.cpu.x[kA] = 1;
    try {
      f.exec(encI(0x13, 0x1, kD, kA, 0x600)); // clz
      ++g_checks;
      ++g_failures;
      std::printf("FAIL %-24s did not trap\n", "isa filter rejects Zbb");
    } catch (const Trap &t) {
      expect("isa filter rejects Zbb",
             static_cast<uint32_t>(t.cause),
             static_cast<uint32_t>(TrapCause::IllegalInstruction));
    }
  }

  std::printf("selftest: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures;
}

} // namespace rsim
