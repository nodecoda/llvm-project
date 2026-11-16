//===-- main.cpp - rmach entry point, chapter 01 ----------------*- C++ -*-===//
//
// Load a bare-metal ELF, reset the machine at its entry point and step until
// ebreak/ecall or the step limit.  --dump-boot prints the same entry/segment
// view chapter 01 compares against llvm-readelf.
//
//===----------------------------------------------------------------------===//

#include "machine.h"

#include "elf.h"
#include "memory.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char *kVersion = "0.1";
constexpr int kExitUsage = 2;
constexpr int kExitLoadError = 125;
constexpr int kExitTrap = 126;

struct Options {
  std::string kernel;
  uint64_t max_steps = 10000000;
  bool trace = false;
  bool dump_boot = false;
};

void usage() {
  std::fprintf(stderr, "usage: rmach --version\n"
                       "       rmach --kernel <elf> [--dump-boot]\n"
                       "       rmach --kernel <elf> [--max-steps <n>] [--trace]\n");
}

bool parse(int argc, char **argv, Options &opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char *what) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "rmach: %s needs an argument\n", what);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--kernel") {
      const char *p = next("--kernel");
      if (!p)
        return false;
      opt.kernel = p;
    } else if (arg == "--max-steps") {
      const char *v = next("--max-steps");
      if (!v)
        return false;
      opt.max_steps = std::strtoull(v, nullptr, 0);
    } else if (arg == "--trace") {
      opt.trace = true;
    } else if (arg == "--dump-boot") {
      opt.dump_boot = true;
    } else if (!arg.empty() && arg[0] == '-') {
      return false;
    } else {
      return false;
    }
  }
  return true;
}

void dumpBoot(const rsim::Image &image) {
  std::printf("entry 0x%08" PRIx32 "\n", image.entry);
  for (const rsim::Segment &seg : image.segments)
    std::printf("load vaddr=0x%08" PRIx32 " filesz=0x%08" PRIx32
                " memsz=0x%08" PRIx32 " flags=0x%x align=0x%x\n",
                seg.vaddr, seg.filesz, seg.memsz, seg.flags, seg.align);
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rmach %s (chapter 01)\n", kVersion);
    return 0;
  }

  Options opt;
  if (!parse(argc, argv, opt) || opt.kernel.empty()) {
    usage();
    return kExitUsage;
  }

  try {
    const rsim::Image image = rsim::loadElf(opt.kernel);

    if (opt.dump_boot) {
      dumpBoot(image);
      return 0;
    }

    rsim::Memory memory;
    rmach::Machine machine;
    machine.reset(image, memory);
    if (opt.trace)
      machine.traceTo(stderr);

    uint64_t steps = 0;
    try {
      while (!machine.halted() && steps < opt.max_steps) {
        machine.step();
        ++steps;
      }
    } catch (const rmach::Trap &err) {
      std::fprintf(stderr, "rmach: trap: %s\n", err.what());
      return kExitTrap;
    }

    if (!machine.halted()) {
      std::fprintf(stderr,
                   "rmach: step limit %" PRIu64 " reached at pc 0x%08" PRIx32
                   "\n",
                   opt.max_steps, machine.pc);
      return kExitTrap;
    }

    std::fprintf(stderr,
                 "rmach: halted at 0x%08" PRIx32 " after %" PRIu64
                 " instructions (exit %d)\n",
                 machine.pc, machine.executed(), machine.exitCode());
    return machine.exitCode();
  } catch (const rsim::ElfError &err) {
    std::fprintf(stderr, "rmach: %s: %s\n", opt.kernel.c_str(), err.what());
    return kExitLoadError;
  } catch (const rsim::MemoryError &err) {
    std::fprintf(stderr, "rmach: trap: %s\n", err.what());
    return kExitTrap;
  }
}
