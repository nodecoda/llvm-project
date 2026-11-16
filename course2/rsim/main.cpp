//===-- main.cpp - rsim entry point, chapter 02 -----------------*- C++ -*-===//
//
// Chapter 02 adds the register file (and the stack that QEMU would otherwise
// set up for us).  There is still no instruction execution: `rsim FILE`
// loads the image, resets the CPU and prints the reset state.
//
// Exit codes: 0 success, 2 usage, 125 load error, 126 memory trap.
//
//===----------------------------------------------------------------------===//

#include "cpu.h"
#include "elf.h"
#include "memory.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char *kVersion = "0.2";
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

void usage() {
  std::fprintf(stderr, "usage: rsim --version\n"
                       "       rsim --dump-elf <elf>\n"
                       "       rsim --dump-mem <elf> <addr> <len>\n"
                       "       rsim <elf>\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rsim %s (chapter 02)\n", kVersion);
    return 0;
  }

  const bool dump_elf = argc == 3 && std::strcmp(argv[1], "--dump-elf") == 0;
  const bool dump_mem = argc == 5 && std::strcmp(argv[1], "--dump-mem") == 0;
  const bool run = argc == 2 && argv[1][0] != '-';

  if (!dump_elf && !dump_mem && !run) {
    usage();
    return kExitUsage;
  }

  const std::string path = argv[2 - (argc == 2 ? 1 : 0)];
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

    std::printf("rsim: loaded %s: entry 0x%08" PRIx32 ", sp 0x%08" PRIx32
                ", %zu segments, %zu pages; execution arrives in chapter 03\n",
                path.c_str(), cpu.pc, cpu.sp(), image.segments.size(),
                memory.mappedPages());
    return 0;
  } catch (const rsim::ElfError &err) {
    std::fprintf(stderr, "rsim: %s: %s\n", path.c_str(), err.what());
    return kExitLoadError;
  } catch (const rsim::MemoryError &err) {
    std::fprintf(stderr, "rsim: trap: %s\n", err.what());
    return kExitTrap;
  }
}
