//===-- main.cpp - rsim entry point, chapter 01 -----------------*- C++ -*-===//
//
// Chapter 01 adds the ELF loader.  There is still no CPU: `rsim FILE` loads
// the image into memory and says so; `rsim --dump-elf FILE` prints exactly
// the fields the verify script compares against llvm-readelf.
//
// Exit codes: 0 success, 2 usage, 125 load error.
//
//===----------------------------------------------------------------------===//

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

void dumpElf(const rsim::Image &image) {
  std::printf("entry 0x%08" PRIx32 "\n", image.entry);
  for (const rsim::Segment &seg : image.segments)
    std::printf("load vaddr=0x%08" PRIx32 " filesz=0x%08" PRIx32
                " memsz=0x%08" PRIx32 " flags=0x%x align=0x%x\n",
                seg.vaddr, seg.filesz, seg.memsz, seg.flags, seg.align);
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("rsim %s (chapter 01)\n", kVersion);
    return 0;
  }

  const bool dump = argc == 3 && std::strcmp(argv[1], "--dump-elf") == 0;
  const bool plain = argc == 2 && argv[1][0] != '-';
  if (!dump && !plain) {
    std::fprintf(stderr, "usage: rsim --version\n"
                         "       rsim --dump-elf <elf>\n"
                         "       rsim <elf>\n");
    return kExitUsage;
  }

  const std::string path = argv[argc - 1];
  try {
    const rsim::Image image = rsim::loadElf(path);

    if (dump) {
      dumpElf(image);
      return 0;
    }

    rsim::Memory memory;
    memory.load(image);
    std::printf("rsim: loaded %s: entry 0x%08" PRIx32
                ", %zu segments, %zu pages; execution arrives in chapter 03\n",
                path.c_str(), image.entry, image.segments.size(),
                memory.mappedPages());
    return 0;
  } catch (const rsim::ElfError &err) {
    std::fprintf(stderr, "rsim: %s: %s\n", path.c_str(), err.what());
    return kExitLoadError;
  } catch (const rsim::MemoryError &err) {
    std::fprintf(stderr, "rsim: %s: %s\n", path.c_str(), err.what());
    return kExitLoadError;
  }
}
