//===-- trap.h - one precise trap or interrupt, chapter 03 ------*- C++ -*-===//
//
// Chapter 01 only needs the synchronous causes; chapters 03-06 add the
// privileged ecall, the page faults and the interrupt bit.  Keeping the full
// numbering here means the machine never has to renumber later.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_TRAP_H
#define RMACH_TRAP_H

#include <cstdint>
#include <stdexcept>
#include <string>

namespace rmach {

enum class TrapCause : uint32_t {
  InstructionAddressMisaligned = 0,
  InstructionAccessFault = 1,
  IllegalInstruction = 2,
  Breakpoint = 3,
  LoadAddressMisaligned = 4,
  LoadAccessFault = 5,
  StoreAddressMisaligned = 6,
  StoreAccessFault = 7,
  EcallFromU = 8,
  EcallFromM = 11,
  InstructionPageFault = 12,
  LoadPageFault = 13,
  StorePageFault = 15,
};

/// The interrupt bit in mcause, per the privileged spec.
constexpr uint32_t kInterruptBit = 0x80000000u;

const char *trapCauseName(TrapCause cause);

struct Trap : std::runtime_error {
  Trap(TrapCause cause, uint32_t tval, uint32_t epc, const std::string &detail);

  TrapCause cause;
  uint32_t tval; ///< the offending address, or the offending instruction word
  uint32_t epc;  ///< the address of the instruction that trapped

private:
  static std::string makeWhat(TrapCause cause, uint32_t tval, uint32_t epc,
                              const std::string &detail);
};

} // namespace rmach

#endif // RMACH_TRAP_H
