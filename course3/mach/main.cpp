//===-- main.cpp - rmach entry point, chapter 01 ----------------*- C++ -*-===//
//
// Load a bare-metal ELF, reset the machine at its entry point and step until
// ebreak/ecall or the step limit.  --user may be repeated: each image becomes
// one U-mode process.  --dump-boot/--dump-procs/--stats expose the boot
// protocol, the process table and the interrupt counters.
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
#include <vector>

namespace {

constexpr const char *kVersion = "1.0";
constexpr int kExitUsage = 2;
constexpr int kExitLoadError = 125;
constexpr int kExitTrap = 126;

struct Options {
  std::string kernel;
  std::vector<std::string> user;
  std::string input;
  uint64_t max_steps = 10000000;
  bool trace = false;
  bool dump_boot = false;
  bool dump_pagetable = false;
  bool dump_procs = false;
  bool dump_csr = false;
  bool stats = false;
  bool have_pagetable_satp = false;
  uint32_t pagetable_satp = 0;
  bool have_pagetable_pid = false;
  uint32_t pagetable_pid = 0;
};

void usage() {
  std::fprintf(stderr, "usage: rmach --version\n"
                       "       rmach --selftest\n"
                       "       rmach --kernel <elf> [--user <elf>]... [--dump-boot]\n"
                       "       rmach --kernel <elf> [--user <elf>]... [--max-steps <n>]\n"
                       "                    [--trace] [--input <file>]\n"
                       "                    [--dump-pagetable [<satp>|pid<N>]]\n"
                       "                    [--dump-procs] [--stats] [--dump-csr]\n");
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
      opt.user.emplace_back(p);
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
    } else if (arg == "--dump-procs") {
      opt.dump_procs = true;
    } else if (arg == "--stats") {
      opt.stats = true;
    } else if (arg == "--dump-csr") {
      opt.dump_csr = true;
    } else if (arg == "--dump-pagetable") {
      opt.dump_pagetable = true;
      // An optional satp argument lets the reader dump a table that the
      // program is no longer using; without it we use the final satp.
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        const std::string value = argv[++i];
        if (value.rfind("pid", 0) == 0) {
          opt.pagetable_pid =
              static_cast<uint32_t>(std::strtoul(value.c_str() + 3, nullptr, 0));
          opt.have_pagetable_pid = true;
        } else {
          opt.pagetable_satp =
              static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
          opt.have_pagetable_satp = true;
        }
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

void dumpBoot(const rsim::Image &image, const std::string &prefix = "") {
  std::printf("%sentry 0x%08" PRIx32 "\n", prefix.c_str(), image.entry);
  for (const rsim::Segment &seg : image.segments)
    std::printf("%sload vaddr=0x%08" PRIx32 " filesz=0x%08" PRIx32
                " memsz=0x%08" PRIx32 " flags=0x%x align=0x%x\n",
                prefix.c_str(), seg.vaddr, seg.filesz, seg.memsz, seg.flags,
                seg.align);
}

const char *stateName(uint32_t state) {
  switch (state) {
  case 0:
    return "RUNNABLE";
  case 1:
    return "RUNNING";
  case 2:
    return "EXITED";
  default:
    return "ABSENT";
  }
}

void dumpProcs(const rsim::Memory &ram) {
  const uint32_t base = rmach::Machine::kBootResult;
  const uint32_t magic = ram.read32(base);
  if (magic != BOOT_RESULT_MAGIC) {
    std::printf("(no process table)\n");
    return;
  }
  const uint32_t nproc = ram.read32(base + 4);
  std::printf("nproc %" PRIu32 "\n", nproc);
  for (uint32_t i = 0; i < BOOT_MAX_PROCS; ++i) {
    const uint32_t off = base + 8 + i * 20;
    const uint32_t pid = ram.read32(off);
    const uint32_t state = ram.read32(off + 4);
    const uint32_t exit_code = ram.read32(off + 8);
    const uint32_t pc = ram.read32(off + 12);
    const uint32_t satp = ram.read32(off + 16);
    std::printf("proc %" PRIu32 " state=%s exit=%" PRId32
                " pc=0x%08" PRIx32 " satp=0x%08" PRIx32 "\n",
                pid, stateName(state), static_cast<int32_t>(exit_code), pc,
                satp);
  }
}

bool procSatp(const rsim::Memory &ram, uint32_t pid, uint32_t &satp) {
  const uint32_t base = rmach::Machine::kBootResult;
  if (pid >= BOOT_MAX_PROCS)
    return false;
  if (ram.read32(base) != BOOT_RESULT_MAGIC)
    return false;
  if (pid >= ram.read32(base + 4))
    return false;
  satp = ram.read32(base + 8 + pid * 20 + 16);
  return true;
}

void dumpCsr(const rmach::Machine &machine) {
  const struct {
    const char *name;
    uint16_t number;
  } names[] = {
      {"mstatus", rmach::kCsrMstatus},   {"misa", rmach::kCsrMisa},
      {"mie", rmach::kCsrMie},           {"mtvec", rmach::kCsrMtvec},
      {"mscratch", rmach::kCsrMscratch}, {"mepc", rmach::kCsrMepc},
      {"mcause", rmach::kCsrMcause},     {"mtval", rmach::kCsrMtval},
      {"mip", rmach::kCsrMip},           {"mhartid", rmach::kCsrMhartid},
      {"mvendorid", rmach::kCsrMvendorid},
      {"marchid", rmach::kCsrMarchid},   {"mimpid", rmach::kCsrMimpid},
      {"minstret", rmach::kCsrMinstret}, {"satp", rmach::kCsrSatp},
  };
  for (const auto &csr : names)
    std::printf("csr %s=0x%08" PRIx32 "\n", csr.name,
                machine.readCsr(csr.number));
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rmach %s (chapter 10)\n", kVersion);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--selftest") == 0)
    return rmach::runSelfTest() == 0 ? 0 : 1;

  Options opt;
  if (!parse(argc, argv, opt) || opt.kernel.empty()) {
    usage();
    return kExitUsage;
  }
  if (opt.user.size() > BOOT_MAX_PROCS) {
    std::fprintf(stderr, "rmach: at most %d --user images\n", BOOT_MAX_PROCS);
    return kExitUsage;
  }

  try {
    const rsim::Image image = rsim::loadElf(opt.kernel);
    std::vector<rsim::Image> user_images;
    user_images.reserve(opt.user.size());
    for (const std::string &path : opt.user) {
      try {
        user_images.push_back(rsim::loadElf(path));
      } catch (const rsim::ElfError &err) {
        std::fprintf(stderr, "rmach: %s: %s\n", path.c_str(), err.what());
        return kExitLoadError;
      }
    }

    if (opt.dump_boot) {
      dumpBoot(image);
      const bool many = user_images.size() > 1;
      for (size_t i = 0; i < user_images.size(); ++i) {
        const std::string prefix =
            many ? "user" + std::to_string(i) + " " : "user ";
        dumpBoot(user_images[i], prefix);
      }
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
    if (user_images.empty())
      machine.setUser(0, rsim::Image{});
    else
      for (size_t i = 0; i < user_images.size(); ++i)
        machine.setUser(static_cast<uint32_t>(i), user_images[i]);
    machine.x[10] = rmach::Machine::kBootInfo; // a0: boot_info[]
    machine.x[11] = static_cast<uint32_t>(user_images.size()); // a1: nproc
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
      uint32_t satp = machine.satp();
      if (opt.have_pagetable_pid) {
        if (!procSatp(bus.ram(), opt.pagetable_pid, satp)) {
          std::fprintf(stderr, "rmach: no page table for pid %" PRIu32 "\n",
                       opt.pagetable_pid);
          return kExitUsage;
        }
      } else if (opt.have_pagetable_satp) {
        satp = opt.pagetable_satp;
      }
      dumpPageTable(bus.ram(), satp);
    }
    if (opt.dump_csr)
      dumpCsr(machine);
    if (opt.dump_procs)
      dumpProcs(bus.ram());
    if (opt.stats)
      std::fprintf(stderr,
                   "rmach: stats: instructions=%" PRIu64 " timer_ticks=%" PRIu32
                   " ecalls=%" PRIu32 "\n",
                   machine.executed(), machine.timerTicks(),
                   machine.ecallCount());

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
