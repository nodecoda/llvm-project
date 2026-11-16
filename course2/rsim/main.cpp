//===-- main.cpp - rsim entry point, chapter 07 -----------------*- C++ -*-===//
//
// Chapter 07 adds `--debug`: a small scriptable debugger.  `--trace` and
// `--disas` are still the batch views of the same information.
//
// Exit codes: 0 success, 2 usage, 125 load error, 126 trap, otherwise the
// guest's own exit status.
//
//===----------------------------------------------------------------------===//

#include "cpu.h"
#include "debugger.h"
#include "decode.h"
#include "elf.h"
#include "memory.h"
#include "selftest.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char *kVersion = "0.7";
constexpr int kExitUsage = 2;
constexpr int kExitLoadError = 125;
constexpr int kExitTrap = 126;

enum class Mode { Reset, Run, Debug, DumpElf, DumpMem, DecodeHex, Disas, SelfTest };

struct Options {
  Mode mode = Mode::Reset;
  std::string path;
  std::string script;
  uint32_t addr = 0;
  uint64_t len = 0;
  uint64_t max_steps = 10000000;
  bool trace = false;
};

void dumpElf(const rsim::Image &image) {
  std::printf("entry 0x%08" PRIx32 "\n", image.entry);
  for (const rsim::Segment &seg : image.segments)
    std::printf("load vaddr=0x%08" PRIx32 " filesz=0x%08" PRIx32
                " memsz=0x%08" PRIx32 " flags=0x%x align=0x%x\n",
                seg.vaddr, seg.filesz, seg.memsz, seg.flags, seg.align);
}

void dumpMem(rsim::Memory &memory, uint32_t addr, uint64_t size) {
  const std::vector<uint8_t> bytes = memory.readBytes(addr, size);
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0)
      std::printf("%s", (i % 16 == 0) ? "\n" : " ");
    std::printf("%02x", bytes[i]);
  }
  std::printf("\n");
}

/// Disassemble every executable PT_LOAD segment.  Output is
/// "<addr> <word> <text>", one instruction per line.
void disas(const rsim::Image &image) {
  for (const rsim::Segment &seg : image.segments) {
    constexpr uint32_t kPfX = 1;
    if (!(seg.flags & kPfX))
      continue;
    for (uint32_t off = 0; off + 4 <= seg.filesz; off += 4) {
      const uint32_t word = static_cast<uint32_t>(seg.data[off]) |
                            static_cast<uint32_t>(seg.data[off + 1]) << 8 |
                            static_cast<uint32_t>(seg.data[off + 2]) << 16 |
                            static_cast<uint32_t>(seg.data[off + 3]) << 24;
      std::printf("%08" PRIx32 " %08" PRIx32 " %s\n", seg.vaddr + off, word,
                  rsim::format(rsim::decode(word)).c_str());
    }
  }
}

/// Decode a file of hex bytes, the same text `llvm-mc -disassemble` accepts:
/// whitespace-separated 0xNN tokens, four bytes per 32-bit instruction.
int decodeHex(const char *path) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "rsim: %s: cannot open file\n", path);
    return kExitUsage;
  }

  std::vector<uint8_t> bytes;
  std::string token;
  while (in >> token) {
    if (token[0] == '#') { // comment to end of line
      in.ignore(1 << 20, '\n');
      continue;
    }
    const char *start = token.c_str();
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X'))
      start += 2;
    char *end = nullptr;
    const unsigned long value = std::strtoul(start, &end, 16);
    if (end == start || *end != '\0' || value > 0xff) {
      std::fprintf(stderr, "rsim: %s: bad byte %s\n", path, token.c_str());
      return kExitUsage;
    }
    bytes.push_back(static_cast<uint8_t>(value));
  }

  if (bytes.empty() || bytes.size() % 4 != 0) {
    std::fprintf(stderr, "rsim: %s: need a whole number of 32-bit words\n",
                 path);
    return kExitUsage;
  }

  for (size_t i = 0; i < bytes.size(); i += 4) {
    const uint32_t word = static_cast<uint32_t>(bytes[i]) |
                          static_cast<uint32_t>(bytes[i + 1]) << 8 |
                          static_cast<uint32_t>(bytes[i + 2]) << 16 |
                          static_cast<uint32_t>(bytes[i + 3]) << 24;
    std::printf("%s\n", rsim::format(rsim::decode(word)).c_str());
  }
  return 0;
}

void usage() {
  std::fprintf(stderr, "usage: rsim --version\n"
                       "       rsim --selftest\n"
                       "       rsim --dump-elf <elf>\n"
                       "       rsim --dump-mem <elf> <addr> <len>\n"
                       "       rsim --decode-hex <file>\n"
                       "       rsim --disas <elf>\n"
                       "       rsim --run <elf> [--max-steps <n>] [--trace]\n"
                       "       rsim --debug <elf> [--script <file>]\n"
                       "       rsim <elf>\n");
}

bool parse(int argc, char **argv, Options &opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char *what) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "rsim: %s needs an argument\n", what);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--run") {
      opt.mode = Mode::Run;
      const char *p = next("--run");
      if (!p)
        return false;
      opt.path = p;
    } else if (arg == "--debug") {
      opt.mode = Mode::Debug;
      const char *p = next("--debug");
      if (!p)
        return false;
      opt.path = p;
    } else if (arg == "--script") {
      const char *p = next("--script");
      if (!p)
        return false;
      opt.script = p;
    } else if (arg == "--dump-elf") {
      opt.mode = Mode::DumpElf;
      const char *p = next("--dump-elf");
      if (!p)
        return false;
      opt.path = p;
    } else if (arg == "--disas") {
      opt.mode = Mode::Disas;
      const char *p = next("--disas");
      if (!p)
        return false;
      opt.path = p;
    } else if (arg == "--decode-hex") {
      opt.mode = Mode::DecodeHex;
      const char *p = next("--decode-hex");
      if (!p)
        return false;
      opt.path = p;
    } else if (arg == "--dump-mem") {
      opt.mode = Mode::DumpMem;
      const char *p = next("--dump-mem");
      if (!p)
        return false;
      opt.path = p;
      const char *a = next("--dump-mem <addr>");
      const char *l = next("--dump-mem <len>");
      if (!a || !l)
        return false;
      opt.addr = static_cast<uint32_t>(std::strtoul(a, nullptr, 0));
      opt.len = std::strtoull(l, nullptr, 0);
    } else if (arg == "--max-steps") {
      const char *v = next("--max-steps");
      if (!v)
        return false;
      opt.max_steps = std::strtoull(v, nullptr, 0);
    } else if (arg == "--trace") {
      opt.trace = true;
    } else if (arg == "--selftest") {
      opt.mode = Mode::SelfTest;
    } else if (!arg.empty() && arg[0] == '-') {
      return false;
    } else if (opt.path.empty()) {
      opt.path = arg; // `rsim FILE` keeps the chapter-02 reset dump
    } else {
      return false;
    }
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rsim %s (chapter 07)\n", kVersion);
    return 0;
  }

  Options opt;
  if (!parse(argc, argv, opt)) {
    usage();
    return kExitUsage;
  }

  if (opt.mode == Mode::SelfTest)
    return rsim::runSelfTest() == 0 ? 0 : 1;

  if (opt.mode == Mode::DecodeHex)
    return decodeHex(opt.path.c_str());

  if (opt.path.empty()) {
    usage();
    return kExitUsage;
  }

  try {
    const rsim::Image image = rsim::loadElf(opt.path);

    if (opt.mode == Mode::DumpElf) {
      dumpElf(image);
      return 0;
    }

    if (opt.mode == Mode::Disas) {
      disas(image);
      return 0;
    }

    rsim::Memory memory;
    rsim::Cpu cpu;
    cpu.reset(image, memory);

    if (opt.mode == Mode::DumpMem) {
      dumpMem(memory, opt.addr, opt.len);
      return 0;
    }

    if (opt.mode == Mode::Reset) {
      std::printf("rsim: loaded %s: entry 0x%08" PRIx32 ", sp 0x%08" PRIx32
                  ", %zu segments, %zu pages\n",
                  opt.path.c_str(), cpu.pc, cpu.sp(), image.segments.size(),
                  memory.mappedPages());
      return 0;
    }

    if (opt.mode == Mode::Debug) {
      std::ifstream script;
      std::istream *input = &std::cin;
      if (!opt.script.empty()) {
        script.open(opt.script);
        if (!script) {
          std::fprintf(stderr, "rsim: %s: cannot open script\n",
                       opt.script.c_str());
          return kExitUsage;
        }
        input = &script;
      }
      rsim::Debugger debugger(cpu, memory, image, opt.max_steps);
      return debugger.run(*input, !opt.script.empty());
    }

    if (opt.trace)
      cpu.traceTo(stderr);

    uint64_t steps = 0;
    while (!cpu.halted() && steps < opt.max_steps) {
      cpu.step();
      ++steps;
    }

    if (!cpu.halted()) {
      std::fprintf(stderr,
                   "rsim: trap: step limit %" PRIu64 " reached at pc 0x%08" PRIx32
                   "\n",
                   opt.max_steps, cpu.pc);
      return kExitTrap;
    }

    // The guest's stdout must stay byte-exact for the QEMU diff, so the
    // summary goes to stderr.  start.S keeps main()'s result in t0 before it
    // calls write(2), which makes t0 the cheapest way to see it.
    std::fprintf(stderr,
                 "rsim: halted at 0x%08" PRIx32 " after %" PRIu64
                 " instructions (exit %d, a0=0x%08" PRIx32 ", t0=0x%08" PRIx32
                 ")\n",
                 cpu.pc, cpu.executed(), cpu.exitCode(), cpu.reg(10), cpu.reg(5));
    return cpu.exitCode();
  } catch (const rsim::ElfError &err) {
    std::fprintf(stderr, "rsim: %s: %s\n", opt.path.c_str(), err.what());
    return kExitLoadError;
  } catch (const rsim::MemoryError &err) {
    std::fprintf(stderr, "rsim: trap: %s\n", err.what());
    return kExitTrap;
  } catch (const rsim::Trap &err) {
    std::fprintf(stderr, "rsim: trap: %s\n", err.what());
    return kExitTrap;
  }
}
