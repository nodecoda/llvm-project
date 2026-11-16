//===-- elf.h - ELF32 executable image, chapter 01 --------------*- C++ -*-===//
//
// Only what the course needs: a 32-bit little-endian RISC-V ET_EXEC file,
// reduced to its entry point and PT_LOAD segments.
//
//===----------------------------------------------------------------------===//

#ifndef RSIM_ELF_H
#define RSIM_ELF_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rsim {

struct ElfError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Segment {
  uint32_t vaddr = 0;
  uint32_t filesz = 0;
  uint32_t memsz = 0;
  uint32_t flags = 0; // PF_X = 1, PF_W = 2, PF_R = 4
  uint32_t align = 0;
  uint32_t file_offset = 0;
  std::vector<uint8_t> data; // exactly filesz bytes
};

struct Symbol {
  std::string name;
  uint32_t value = 0;
  uint32_t size = 0;
  uint8_t type = 0; // STT_FUNC = 2, STT_OBJECT = 1
};

struct Image {
  uint32_t entry = 0;
  std::vector<Segment> segments;
  std::vector<Symbol> symbols; // from .symtab, chapter 07
};

/// Parse `path`, or throw ElfError with a human-readable reason.
/// The loader rejects everything the emulator cannot model (ET_DYN, PT_INTERP,
/// PT_DYNAMIC, truncated headers or segment data, overflowing ranges).
Image loadElf(const std::string &path);

} // namespace rsim

#endif // RSIM_ELF_H
