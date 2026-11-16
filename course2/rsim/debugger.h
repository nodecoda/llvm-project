//===-- debugger.h - a scriptable breakpoint/step debugger, chapter 07 ----===//
//
// The command set is deliberately small and the output deliberately stable:
// a script plus its transcript is the gate, so "the debugger works" has a
// byte-level definition.
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_DEBUGGER_H
#define RSIM_DEBUGGER_H

#include "cpu.h"
#include "elf.h"
#include "memory.h"

#include <cstdint>
#include <istream>
#include <map>
#include <string>

namespace rsim {

class Debugger {
public:
  Debugger(Cpu &cpu, Memory &memory, const Image &image, uint64_t max_steps)
      : cpu_(cpu), memory_(memory), image_(image), max_steps_(max_steps) {}

  /// Read commands until `q` or end of input.  Returns the guest's exit
  /// status when the program finished, otherwise 0.
  int run(std::istream &in, bool echo);

private:
  bool command(const std::string &line);
  void doBreak(const std::string &target);
  void doContinue();
  void doStep(uint64_t count);
  void doRegisters() const;
  void doExamine(uint32_t addr, unsigned words) const;
  void reportStop();
  void reportTrap(const Trap &trap);
  std::string describe(uint32_t pc) const;
  bool resolve(const std::string &token, uint32_t &addr) const;

  Cpu &cpu_;
  Memory &memory_;
  const Image &image_;
  uint64_t max_steps_;
  std::map<uint32_t, int> breakpoints_;
  uint32_t last_stop_pc_ = 0;
  bool have_stop_ = false;
  bool trapped_ = false;
};

} // namespace rsim

#endif // RSIM_DEBUGGER_H
