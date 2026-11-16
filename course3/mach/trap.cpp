//===-- trap.cpp - human-readable trap names, chapter 03 -------------------===//

#include "trap.h"

#include <cstdio>

namespace rmach {

const char *trapCauseName(TrapCause cause) {
  switch (cause) {
  case TrapCause::InstructionAddressMisaligned:
    return "instruction address misaligned";
  case TrapCause::InstructionAccessFault:
    return "instruction access fault";
  case TrapCause::IllegalInstruction:
    return "illegal instruction";
  case TrapCause::Breakpoint:
    return "breakpoint";
  case TrapCause::LoadAddressMisaligned:
    return "load address misaligned";
  case TrapCause::LoadAccessFault:
    return "load access fault";
  case TrapCause::StoreAddressMisaligned:
    return "store address misaligned";
  case TrapCause::StoreAccessFault:
    return "store access fault";
  case TrapCause::EcallFromU:
    return "ecall from U-mode";
  case TrapCause::EcallFromM:
    return "ecall from M-mode";
  case TrapCause::InstructionPageFault:
    return "instruction page fault";
  case TrapCause::LoadPageFault:
    return "load page fault";
  case TrapCause::StorePageFault:
    return "store page fault";
  }
  return "unknown trap";
}

Trap::Trap(TrapCause cause, uint32_t tval, uint32_t epc,
           const std::string &detail)
    : std::runtime_error(makeWhat(cause, tval, epc, detail)), cause(cause),
      tval(tval), epc(epc) {}

std::string Trap::makeWhat(TrapCause cause, uint32_t tval, uint32_t epc,
                           const std::string &detail) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s: tval=0x%08x epc=0x%08x (%s)",
                trapCauseName(cause), tval, epc, detail.c_str());
  return buf;
}

} // namespace rmach
