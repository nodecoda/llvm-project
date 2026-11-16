//===-- trap.cpp - trap formatting, chapter 04 ----------------------------===//

#include "trap.h"

#include <cstdio>

namespace rsim {

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
  }
  return "unknown trap";
}

namespace {
std::string describe(TrapCause cause, uint32_t tval, uint32_t epc,
                     const std::string &detail) {
  char buf[320];
  std::snprintf(buf, sizeof(buf), "%s: tval=0x%08x epc=0x%08x%s%s%s",
                trapCauseName(cause), tval, epc, detail.empty() ? "" : " (",
                detail.c_str(), detail.empty() ? "" : ")");
  return buf;
}
} // namespace

Trap::Trap(TrapCause cause, uint32_t tval, uint32_t epc,
           const std::string &detail)
    : std::runtime_error(describe(cause, tval, epc, detail)), cause(cause),
      tval(tval), epc(epc) {}

} // namespace rsim
