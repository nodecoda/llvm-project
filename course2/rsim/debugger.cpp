//===-- debugger.cpp - breakpoints, single step, registers, memory, ch 07 ===//
//
// Transcripts are the contract, so every message here is deterministic: no
// addresses that depend on the host, no timing, no prompts that vary.
//
//===----------------------------------------------------------------------===//

#include "debugger.h"

#include "decode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace rsim {
namespace {

bool isNumber(const std::string &token) {
  return !token.empty() && (std::isdigit(static_cast<unsigned char>(token[0])) ||
                            token[0] == '-');
}

} // namespace

int Debugger::run(std::istream &in, bool echo) {
  std::string line;
  for (;;) {
    std::printf("(rsim) ");
    std::fflush(stdout);
    if (!std::getline(in, line)) {
      std::printf("\n");
      break;
    }
    if (echo)
      std::printf("%s\n", line.c_str());
    if (!command(line))
      break;
  }
  return cpu_.halted() ? cpu_.exitCode() : 0;
}

bool Debugger::command(const std::string &line) {
  std::istringstream in(line);
  std::string cmd;
  if (!(in >> cmd))
    return true; // empty line

  if (cmd == "q" || cmd == "quit") {
    std::printf("quit\n");
    return false;
  }
  if (cmd == "b" || cmd == "break") {
    std::string target;
    if (!(in >> target)) {
      std::printf("usage: b <symbol|address>\n");
      return true;
    }
    doBreak(target);
    return true;
  }
  if (cmd == "c" || cmd == "continue") {
    doContinue();
    return true;
  }
  if (cmd == "s" || cmd == "step") {
    uint64_t count = 1;
    if (!(in >> count))
      count = 1;
    doStep(count);
    return true;
  }
  if (cmd == "r" || cmd == "registers") {
    doRegisters();
    return true;
  }
  if (cmd.size() > 4 && cmd.compare(0, 2, "x/") == 0 &&
      cmd.compare(cmd.size() - 2, 2, "xw") == 0) {
    const unsigned count =
        static_cast<unsigned>(std::strtoul(cmd.substr(2, cmd.size() - 4).c_str(),
                                           nullptr, 10));
    std::string addr;
    if (!(in >> addr) || count == 0) {
      std::printf("usage: x/<count>xw <address>\n");
      return true;
    }
    uint32_t value = 0;
    if (!resolve(addr, value)) {
      std::printf("cannot resolve address %s\n", addr.c_str());
      return true;
    }
    doExamine(value, count);
    return true;
  }

  std::printf("undefined command: %s\n", cmd.c_str());
  return true;
}

bool Debugger::resolve(const std::string &token, uint32_t &addr) const {
  if (isNumber(token)) {
    addr = static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 0));
    return true;
  }
  for (unsigned i = 0; i < 32; ++i)
    if (token == registerName(i)) {
      addr = cpu_.reg(i);
      return true;
    }
  for (const Symbol &sym : image_.symbols)
    if (sym.name == token) {
      addr = sym.value;
      return true;
    }
  return false;
}

std::string Debugger::describe(uint32_t pc) const {
  const Symbol *best = nullptr;
  for (const Symbol &sym : image_.symbols)
    if (sym.value <= pc && (!best || sym.value > best->value))
      best = &sym;
  if (!best)
    return "";
  if (pc == best->value)
    return " <" + best->name + ">";
  char buf[96];
  std::snprintf(buf, sizeof(buf), " <%s+%u>", best->name.c_str(),
                pc - best->value);
  return buf;
}

void Debugger::doBreak(const std::string &target) {
  uint32_t addr = 0;
  if (!resolve(target, addr)) {
    std::printf("cannot resolve %s\n", target.c_str());
    return;
  }
  const int id = static_cast<int>(breakpoints_.size());
  breakpoints_[addr] = id;
  std::printf("breakpoint %d: 0x%08x%s\n", id, addr, describe(addr).c_str());
}

void Debugger::doContinue() {
  if (trapped_) {
    std::printf("program has trapped; cannot continue\n");
    return;
  }
  uint64_t steps = 0;
  // Resuming from a reported stop at a breakpoint must execute that
  // instruction once; setting a breakpoint on the current pc and continuing
  // must stop before it.
  bool skip_once = have_stop_ && cpu_.pc == last_stop_pc_;
  while (!cpu_.halted() && steps < max_steps_) {
    if (breakpoints_.count(cpu_.pc) && !skip_once)
      break;
    skip_once = false;
    try {
      cpu_.step();
    } catch (const Trap &trap) {
      reportTrap(trap);
      return;
    }
    ++steps;
  }
  if (cpu_.halted()) {
    std::printf("program exited with status %d\n", cpu_.exitCode());
    return;
  }
  reportStop();
}

void Debugger::doStep(uint64_t count) {
  if (trapped_) {
    std::printf("program has trapped; cannot continue\n");
    return;
  }
  for (uint64_t i = 0; i < count && !cpu_.halted(); ++i) {
    try {
      cpu_.step();
    } catch (const Trap &trap) {
      reportTrap(trap);
      return;
    }
  }
  if (cpu_.halted()) {
    std::printf("program exited with status %d\n", cpu_.exitCode());
    return;
  }
  reportStop();
}

void Debugger::reportStop() {
  std::printf("stopped at 0x%08x%s\n", cpu_.pc, describe(cpu_.pc).c_str());
  last_stop_pc_ = cpu_.pc;
  have_stop_ = true;
}

void Debugger::reportTrap(const Trap &trap) {
  trapped_ = true;
  std::printf("program trapped: %s\n", trap.what());
}

void Debugger::doRegisters() const {
  for (unsigned i = 0; i < 32; ++i) {
    if (i % 4 == 0)
      std::printf("%s", i == 0 ? "" : "\n");
    std::printf("x%u(%s)=0x%08x%s", i, registerName(i), cpu_.reg(i),
                i % 4 == 3 ? "" : "  ");
  }
  std::printf("\n");
}

void Debugger::doExamine(uint32_t addr, unsigned words) const {
  for (unsigned i = 0; i < words; ++i) {
    if (i % 4 == 0)
      std::printf("%s0x%08x:", i == 0 ? "" : "\n", addr + 4 * i);
    try {
      std::printf(" 0x%08x", memory_.read32(addr + 4 * i));
    } catch (const MemoryError &err) {
      std::printf(" <%s>\n", err.what());
      return;
    }
  }
  std::printf("\n");
}

} // namespace rsim
