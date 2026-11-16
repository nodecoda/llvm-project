//===-- syscall.cpp - the two system calls the course runtime uses, ch 05 ===//
//
// Linux user-mode RV32 passes the call number in a7 and the arguments in
// a0-a2; the return value goes back in a0.  Course 1's runtime only ever asks
// for write(2) and exit(2), so those are the two we implement.  Anything else
// stops with exit code 127 instead of pretending to succeed: a silently
// ignored syscall is a bug that hides until much later.
//
//===----------------------------------------------------------------------===//

#include "cpu.h"

#include <cstdio>

namespace rsim {

bool Cpu::ecall(uint32_t at) {
  constexpr uint32_t kSysWrite = 64;
  constexpr uint32_t kSysExit = 93;

  const uint32_t number = x[17]; // a7

  if (number == kSysWrite) {
    const uint32_t fd = x[10]; // a0
    if (fd != 1 && fd != 2) {
      x[10] = 0xffffffffu; // -EBADF, and keep running
      return true;
    }
    std::FILE *out = fd == 1 ? stdout : stderr;
    const uint32_t buf = x[11]; // a1
    const uint32_t len = x[12]; // a2
    // Byte at a time, through the same checked path as a guest load: a bad
    // buffer is a precise load trap, not a host segfault.
    for (uint32_t i = 0; i < len; ++i)
      std::fputc(load8(buf + i, at), out);
    x[10] = len;
    return true;
  }

  if (number == kSysExit) {
    halted_ = true;
    exit_code_ = static_cast<int>(x[10] & 0xff);
    return false;
  }

  std::fprintf(stderr, "rsim: trap: unknown ecall a7=%u at 0x%08x\n", number,
               at);
  halted_ = true;
  exit_code_ = 127;
  return false;
}

} // namespace rsim
