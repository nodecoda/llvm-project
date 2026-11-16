//===-- elf.cpp - ELF32 executable loader, chapter 01 ---------------------===//

#include "elf.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace rsim {
namespace {

constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t EM_RISCV = 243;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_DYNAMIC = 2;
constexpr uint32_t PT_INTERP = 3;
constexpr uint32_t PT_NULL = 0;

uint16_t read16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

Image loadElf(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw ElfError("cannot open file");

  std::vector<uint8_t> buf((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

  if (buf.size() < 52)
    throw ElfError("truncated ELF header (need 52 bytes)");
  if (std::memcmp(buf.data(), "\x7f" "ELF", 4) != 0)
    throw ElfError("bad magic (not an ELF file)");
  if (buf[4] != 1)
    throw ElfError("not a 32-bit ELF (EI_CLASS != ELFCLASS32)");
  if (buf[5] != 1)
    throw ElfError("not little-endian (EI_DATA != ELFDATA2LSB)");
  if (buf[6] != 1)
    throw ElfError("unsupported ELF version");

  const uint16_t type = read16(&buf[16]);
  const uint16_t machine = read16(&buf[18]);
  const uint32_t version = read32(&buf[20]);
  if (version != 1)
    throw ElfError("unsupported e_version");
  if (machine != EM_RISCV)
    throw ElfError("not a RISC-V ELF (e_machine != 243)");
  if (type != ET_EXEC)
    throw ElfError("only static executables are supported (e_type != ET_EXEC)");

  Image image;
  image.entry = read32(&buf[24]);
  const uint32_t phoff = read32(&buf[28]);
  const uint16_t phentsize = read16(&buf[42]);
  const uint16_t phnum = read16(&buf[44]);

  if (phnum == 0)
    throw ElfError("no program headers");
  if (phentsize < 32)
    throw ElfError("program header entry is smaller than 32 bytes");
  const uint64_t ph_end =
      static_cast<uint64_t>(phoff) + static_cast<uint64_t>(phnum) * phentsize;
  if (ph_end > buf.size())
    throw ElfError("program header table is truncated");

  for (uint16_t i = 0; i < phnum; ++i) {
    const uint8_t *ph = &buf[phoff + static_cast<uint32_t>(i) * phentsize];
    const uint32_t p_type = read32(ph);
    const uint32_t offset = read32(ph + 4);
    const uint32_t vaddr = read32(ph + 8);
    const uint32_t filesz = read32(ph + 16);
    const uint32_t memsz = read32(ph + 20);
    const uint32_t flags = read32(ph + 24);
    const uint32_t align = read32(ph + 28);

    if (p_type == PT_INTERP)
      throw ElfError("PT_INTERP is not supported (static executables only)");
    if (p_type == PT_DYNAMIC)
      throw ElfError("PT_DYNAMIC is not supported (static executables only)");
    if (p_type == PT_NULL || p_type != PT_LOAD)
      continue;

    if (filesz > memsz)
      throw ElfError("PT_LOAD has p_filesz > p_memsz");
    if (static_cast<uint64_t>(offset) + filesz > buf.size())
      throw ElfError("PT_LOAD file range is truncated");
    if (static_cast<uint64_t>(vaddr) + memsz > (uint64_t{1} << 32))
      throw ElfError("PT_LOAD virtual range overflows 32 bits");
    if (align != 0 && (align & (align - 1)) != 0)
      throw ElfError("PT_LOAD p_align is not a power of two");

    Segment seg;
    seg.vaddr = vaddr;
    seg.filesz = filesz;
    seg.memsz = memsz;
    seg.flags = flags;
    seg.align = align;
    seg.file_offset = offset;
    seg.data.assign(buf.begin() + offset, buf.begin() + offset + filesz);
    image.segments.push_back(std::move(seg));
  }

  if (image.segments.empty())
    throw ElfError("no PT_LOAD segments");

  bool entry_in_segment = false;
  for (const Segment &seg : image.segments) {
    if (image.entry >= seg.vaddr &&
        static_cast<uint64_t>(image.entry) <
            static_cast<uint64_t>(seg.vaddr) + seg.memsz)
      entry_in_segment = true;
  }
  if (!entry_in_segment)
    throw ElfError("entry point is outside every PT_LOAD segment");

  return image;
}

} // namespace rsim
