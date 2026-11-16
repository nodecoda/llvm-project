//===-- main.cpp - rmach entry point, chapter 01 ----------------*- C++ -*-===//
//
// Load a bare-metal ELF, reset the machine at its entry point and step until
// ebreak/ecall or the step limit.  --dump-boot prints the same entry/segment
// view chapter 01 compares against llvm-readelf.
//
//===----------------------------------------------------------------------===//

#include "machine.h"
#include "selftest.h"

#include "elf.h"
#include "memory.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr const char *kVersion = "0.7";
constexpr int kExitUsage = 2;
constexpr int kExitLoadError = 125;
constexpr int kExitTrap = 126;

struct Options {
  std::string kernel;
  std::string user;
  std::string input;
  uint64_t max_steps = 10000000;
  bool trace = false;
  bool dump_boot = false;
  bool dump_pagetable = false;
  bool have_pagetable_satp = false;
  uint32_t pagetable_satp = 0;
};

void usage() {
  std::fprintf(stderr, "usage: rmach --version\n"
                       "       rmach --selftest\n"
                       "       rmach --kernel <elf> [--user <elf>] [--dump-boot]\n"
                       "       rmach --kernel <elf> [--user <elf>] [--max-steps <n>]\n"
                       "                    [--trace] [--input <file>]\n"
                       "                    [--dump-pagetable [<satp>]]\n");
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
    } else if (arg == "--user") {
      const char *p = next("--user");
      if (!p)
        return false;
      opt.user = p;
    } else if (arg == "--input") {
      const char *p = next("--input");
      if (!p)
        return false;
      opt.input = p;
    } else if (arg == "--max-steps") {
      const char *v = next("--max-steps");
      if (!v)
        return false;
      opt.max_steps = std::strtoull(v, nullptr, 0);
    } else if (arg == "--trace") {
      opt.trace = true;
    } else if (arg == "--dump-boot") {
      opt.dump_boot = true;
    } else if (arg == "--dump-pagetable") {
      opt.dump_pagetable = true;
      // An optional satp argument lets the reader dump a table that the
      // program is no longer using; without it we use the final satp.
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        opt.pagetable_satp =
            static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        opt.have_pagetable_satp = true;
      }
    } else if (!arg.empty() && arg[0] == '-') {
      return false;
    } else {
      return false;
    }
  }
  return true;
}

void dumpPageTable(const rsim::Memory &ram, uint32_t satp) {
  std::printf("satp 0x%08" PRIx32 " mode=%u asid=0x%03" PRIx32
              " ppn=0x%06" PRIx32 "\n",
              satp, static_cast<unsigned>(satp >> 31), (satp >> 22) & 0x1ff,
              satp & 0x3fffff);
  if ((satp & 0x80000000u) == 0) {
    std::printf("bare: no page table\n");
    return;
  }

  const auto leaves = rmach::Mmu::walk(ram, satp);
  if (leaves.empty()) {
    std::printf("(no valid leaves)\n");
    return;
  }

  constexpr size_t kMaxLeaves = 64;
  for (size_t i = 0; i < leaves.size() && i < kMaxLeaves; ++i) {
    const rmach::Mmu::Leaf &leaf = leaves[i];
    char flags[9];
    size_t n = 0;
    const struct {
      uint32_t bit;
      char ch;
    } names[] = {{rmach::Mmu::kPteV, 'V'}, {rmach::Mmu::kPteR, 'R'},
                 {rmach::Mmu::kPteW, 'W'}, {rmach::Mmu::kPteX, 'X'},
                 {rmach::Mmu::kPteU, 'U'}, {rmach::Mmu::kPteG, 'G'},
                 {rmach::Mmu::kPteA, 'A'}, {rmach::Mmu::kPteD, 'D'}};
    for (const auto &flag : names)
      if (leaf.pte & flag.bit)
        flags[n++] = flag.ch;
    flags[n] = '\0';
    std::printf("leaf va=0x%08" PRIx32 " pa=0x%08" PRIx32
                " pte=0x%08" PRIx32 " level=%d flags=%s\n",
                leaf.va, leaf.pa, leaf.pte, leaf.level, flags);
  }
  if (leaves.size() > kMaxLeaves)
    std::printf("... %zu more leaves\n", leaves.size() - kMaxLeaves);
}

void dumpBoot(const rsim::Image &image, const char *prefix = "") {
  std::printf("%sentry 0x%08" PRIx32 "\n", prefix, image.entry);
  for (const rsim::Segment &seg : image.segments)
    std::printf("%sload vaddr=0x%08" PRIx32 " filesz=0x%08" PRIx32
                " memsz=0x%08" PRIx32 " flags=0x%x align=0x%x\n",
                prefix, seg.vaddr, seg.filesz, seg.memsz, seg.flags, seg.align);
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rmach %s (chapter 07)\n", kVersion);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
    return rmach::runSelfTest() == 0 ? 0 : 1;

  Options opt;
  if (!parse(argc, argv, opt) || opt.kernel.empty()) {
    usage();
    return kExitUsage;
  }

  try {
    const rsim::Image image = rsim::loadElf(opt.kernel);
    rsim::Image user_image;
    const bool has_user = !opt.user.empty();
    if (has_user)
      user_image = rsim::loadElf(opt.user);

    if (opt.dump_boot) {
      dumpBoot(image);
      if (has_user)
        dumpBoot(user_image, "user ");
      return 0;
    }

    std::unique_ptr<std::FILE, int (*)(std::FILE *)> input(nullptr, std::fclose);
    if (!opt.input.empty()) {
      std::FILE *file = std::fopen(opt.input.c_str(), "rb");
      if (!file) {
        std::fprintf(stderr, "rmach: %s: cannot open input\n",
                     opt.input.c_str());
        return kExitUsage;
      }
      input.reset(file);
    }

    rmach::Bus bus;
    bus.uart().setInput(input.get());
    rmach::Machine machine;
    machine.reset(image, bus);
    if (has_user)
      machine.setUser(user_image);
    else
      machine.setUser(rsim::Image{});
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

    if (opt.dump_pagetable) {
      const uint32_t satp = opt.have_pagetable_satp ? opt.pagetable_satp
                                                    : machine.satp();
      dumpPageTable(bus.ram(), satp);
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
