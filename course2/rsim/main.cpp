//===-- main.cpp - rsim entry point, chapter 03 -----------------*- C++ -*-===//
//
// Chapter 03 adds execution: `--run` drives the step loop, `--decode-hex`
// decodes the same hex text `llvm-mc -disassemble` reads, and `--selftest`
// runs the hand-computed instruction vectors.
//
// Exit codes: 0 success, 2 usage, 125 load error, 126 trap.
//
//===----------------------------------------------------------------------===//

#include "cpu.h"
#include "decode.h"
#include "elf.h"
#include "memory.h"
#include "selftest.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr const char *kVersion = "0.3";
constexpr int kExitUsage = 2;
constexpr int kExitLoadError = 125;
constexpr int kExitTrap = 126;

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
                       "       rsim --run <elf> [--max-steps <n>]\n"
                       "       rsim <elf>\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rsim %s (chapter 03)\n", kVersion);
    return 0;
  }

  if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
    return rsim::runSelfTest() == 0 ? 0 : 1;

  if (argc == 3 && std::strcmp(argv[1], "--decode-hex") == 0)
    return decodeHex(argv[2]);

  const bool dump_elf = argc == 3 && std::strcmp(argv[1], "--dump-elf") == 0;
  const bool dump_mem = argc == 5 && std::strcmp(argv[1], "--dump-mem") == 0;
  const bool run = argc >= 3 && std::strcmp(argv[1], "--run") == 0;
  const bool plain = argc == 2 && argv[1][0] != '-';

  if (!dump_elf && !dump_mem && !run && !plain) {
    usage();
    return kExitUsage;
  }

  uint64_t max_steps = 10000000;
  if (run && argc > 3) {
    if (argc != 5 || std::strcmp(argv[3], "--max-steps") != 0) {
      usage();
      return kExitUsage;
    }
    max_steps = std::strtoull(argv[4], nullptr, 0);
  }

  const std::string path = argv[run ? 2 : (argc == 2 ? 1 : 2)];
  uint32_t mem_addr = 0;
  uint64_t mem_len = 0;
  if (dump_mem) {
    mem_addr = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
    mem_len = std::strtoull(argv[4], nullptr, 0);
  }

  try {
    const rsim::Image image = rsim::loadElf(path);

    if (dump_elf) {
      dumpElf(image);
      return 0;
    }

    rsim::Memory memory;
    rsim::Cpu cpu;
    cpu.reset(image, memory);

    if (dump_mem) {
      dumpMem(memory, mem_addr, mem_len);
      return 0;
    }

    if (!run) {
      std::printf("rsim: loaded %s: entry 0x%08" PRIx32 ", sp 0x%08" PRIx32
                  ", %zu segments, %zu pages\n",
                  path.c_str(), cpu.pc, cpu.sp(), image.segments.size(),
                  memory.mappedPages());
      return 0;
    }

    uint64_t steps = 0;
    while (!cpu.halted() && steps < max_steps) {
      cpu.step();
      ++steps;
    }

    if (!cpu.halted()) {
      std::fprintf(stderr,
                   "rsim: trap: step limit %" PRIu64 " reached at pc 0x%08" PRIx32
                   "\n",
                   max_steps, cpu.pc);
      return kExitTrap;
    }

    // start.S keeps main()'s result in t0 before it calls write(2).
    std::printf("rsim: halted at 0x%08" PRIx32 " after %" PRIu64
                " instructions (a0=0x%08" PRIx32 ", t0=0x%08" PRIx32 ")\n",
                cpu.pc, cpu.executed(), cpu.reg(10), cpu.reg(5));
    return 0;
  } catch (const rsim::ElfError &err) {
    std::fprintf(stderr, "rsim: %s: %s\n", path.c_str(), err.what());
    return kExitLoadError;
  } catch (const rsim::MemoryError &err) {
    std::fprintf(stderr, "rsim: trap: %s\n", err.what());
    return kExitTrap;
  } catch (const rsim::Trap &err) {
    std::fprintf(stderr, "rsim: trap: %s\n", err.what());
    return kExitTrap;
  }
}
