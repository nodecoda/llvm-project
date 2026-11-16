//===-- trap.h - one precise trap, chapter 04 -------------------*- C++ -*-===//
//
// A trap is not "an error string": it is the pair (cause, tval) plus the
// address of the instruction that caused it.  Chapter 08 will copy these
// three fields into mcause/mtval/mepc; starting them now keeps the ordering
// honest ("the pc that faulted", never "the pc after the fault").
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_TRAP_H
#define RSIM_TRAP_H

#include <cstdint>
#include <stdexcept>
#include <string>

namespace rsim {

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
};

const char *trapCauseName(TrapCause cause);

struct Trap : std::runtime_error {
  Trap(TrapCause cause, uint32_t tval, uint32_t epc, const std::string &detail);

  TrapCause cause;
  uint32_t tval; ///< the offending address, or the offending instruction word
  uint32_t epc;  ///< the address of the instruction that trapped
};

} // namespace rsim

#endif // RSIM_TRAP_H
