//===-- csr.h - CSR numbers, masks and privilege, chapter 02 -----*- C++ -*-===//
//
// The CSR address itself says who may touch it: bits 9:8 == 0b11 means
// machine mode.  Chapter 02 implements only the registers the course uses,
// with explicit write masks so that a stray 0xffffffff does not corrupt state.
//
//===----------------------------------------------------------------------===//

#ifndef RMACH_CSR_H
#define RMACH_CSR_H

#include <cstdint>

namespace rmach {

enum class Priv : uint32_t { U = 0, M = 3 };

constexpr uint16_t kCsrMstatus = 0x300;
constexpr uint16_t kCsrMisa = 0x301;
constexpr uint16_t kCsrMie = 0x304;
constexpr uint16_t kCsrMtvec = 0x305;
constexpr uint16_t kCsrMscratch = 0x340;
constexpr uint16_t kCsrMepc = 0x341;
constexpr uint16_t kCsrMcause = 0x342;
constexpr uint16_t kCsrMtval = 0x343;
constexpr uint16_t kCsrMip = 0x344;
constexpr uint16_t kCsrMhartid = 0xf14;
constexpr uint16_t kCsrMvendorid = 0xf11;
constexpr uint16_t kCsrMarchid = 0xf12;
constexpr uint16_t kCsrMimpid = 0xf13;
constexpr uint16_t kCsrMinstret = 0xb02;
constexpr uint16_t kCsrSatp = 0x180;

// mstatus: MIE, MPIE and MPP are enough for chapters 03-08.
constexpr uint32_t kMstatusMIE = 1u << 3;
constexpr uint32_t kMstatusMPIE = 1u << 7;
constexpr uint32_t kMstatusMPP = 3u << 11;
constexpr uint32_t kMstatusMask = kMstatusMIE | kMstatusMPIE | kMstatusMPP;

// mie/mip: only the machine timer interrupt exists in this course.
constexpr uint32_t kMTIE = 1u << 7;
constexpr uint32_t kMTIP = 1u << 7;

// satp: MODE(31), ASID(30:22), PPN(21:0).
constexpr uint32_t kSatpMode = 0x80000000u;
constexpr uint32_t kSatpMask = 0x803fffffu;

/// U-mode may only touch unprivileged CSRs (bits 9:8 == 0).
inline bool isPrivilegedCsr(uint16_t csr) { return ((csr >> 8) & 3u) != 0u; }

} // namespace rmach

#endif // RMACH_CSR_H
