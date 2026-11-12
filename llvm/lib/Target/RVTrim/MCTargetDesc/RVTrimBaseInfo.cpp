//===-- RVTrimBaseInfo.cpp - Top level definitions for RISC-V MC -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains small standalone enum definitions for the RISC-V target
// useful for the compiler back-end and the MC libraries.
//
//===----------------------------------------------------------------------===//

#include "RVTrimBaseInfo.h"
#include "RVTrimMCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm {

namespace RVTrimSysReg {
#define GET_SysRegsList_IMPL
#include "RVTrimGenSearchableTables.inc"
} // namespace RVTrimSysReg

namespace RVTrimInsnOpcode {
#define GET_RVTrimOpcodesList_IMPL
#include "RVTrimGenSearchableTables.inc"
} // namespace RVTrimInsnOpcode

namespace RVTrimVInversePseudosTable {
using namespace RVTrim;
#define GET_RVTrimVInversePseudosTable_IMPL
#include "RVTrimGenSearchableTables.inc"
} // namespace RVTrimVInversePseudosTable

namespace RVTrim {
#define GET_RVTrimVSSEGTable_IMPL
#define GET_RVTrimVLSEGTable_IMPL
#define GET_RVTrimVLXSEGTable_IMPL
#define GET_RVTrimVSXSEGTable_IMPL
#define GET_RVTrimVLETable_IMPL
#define GET_RVTrimVSETable_IMPL
#define GET_RVTrimVLXTable_IMPL
#define GET_RVTrimVSXTable_IMPL
#define GET_RVTrimNDSVLNTable_IMPL
#include "RVTrimGenSearchableTables.inc"
} // namespace RVTrim

namespace RVTrimABI {
Expected<ABI> computeTargetABI(const MCSubtargetInfo &STI, StringRef ABIName) {
  const Triple &TT = STI.getTargetTriple();
  const FeatureBitset &FeatureBits = STI.getFeatureBits();
  auto TargetABI = getTargetABI(ABIName);
  bool IsRV64 = TT.isArch64Bit();
  bool IsRVE = FeatureBits[RVTrim::FeatureStdExtE];
  bool IsXCheriot = FeatureBits[RVTrim::FeatureVendorXCheriot];

  if (!ABIName.empty() && TargetABI == ABI_Unknown) {
    return createStringError(Twine("'") + ABIName +
                             "' is not a recognized ABI for this target");
  }
  if (IsRV64 &&
      (ABIName.starts_with("ilp32") || ABIName.starts_with("il32pc64"))) {
    return createStringError(
        "32-bit ABIs are not supported for 64-bit targets");
  }
  if (!IsRV64 &&
      (ABIName.starts_with("lp64") || ABIName.starts_with("l64pc128"))) {
    return createStringError(
        "64-bit ABIs are not supported for 32-bit targets");
  }
  if (ABIName.ends_with('f') && !FeatureBits[RVTrim::FeatureStdExtF]) {
    return createStringError(
        "hard-float 'f' ABI can't be used for a target that doesn't "
        "support the F instruction set extension");
  }
  if (ABIName.ends_with('d') && !FeatureBits[RVTrim::FeatureStdExtD]) {
    return createStringError(
        "hard-float 'd' ABI can't be used for a target that doesn't "
        "support the D instruction set extension");
  }
  if (!FeatureBits[RVTrim::FeatureStdExtY] &&
      (ABIName.starts_with("il32pc64") || ABIName.starts_with("l64pc128"))) {
    return createStringError(Twine('\'') + ABIName +
                             "' ABI is only supported for RVY targets");
  }
  if (!IsRV64 && IsRVE && !IsXCheriot && TargetABI != ABI_ILP32E &&
      TargetABI != ABI_Unknown) {
    return createStringError("only the ilp32e ABI is supported for RV32E");
  }
  if (!IsRV64 && IsRVE && IsXCheriot && TargetABI != ABI_CHERIOT &&
      TargetABI != ABI_Unknown) {
    return createStringError("only the cheriot ABI is supported for XCheriot");
  }
  if (IsRV64 && IsRVE && TargetABI != ABI_LP64E && TargetABI != ABI_Unknown) {
    return createStringError("only the lp64e ABI is supported for RV64E");
  }

  // Unconditionally fatal: no sensible default ABI to fall back to here.
  if ((TargetABI == ABI_ILP32E ||
       (TargetABI == ABI_Unknown && IsRVE && !IsRV64)) &&
      FeatureBits[RVTrim::FeatureStdExtD])
    reportFatalUsageError("ILP32E cannot be used with the D ISA extension");

  if (TargetABI != ABI_Unknown)
    return TargetABI;

  // If no explicit ABI is given, try to compute the default ABI.
  auto ISAInfo = RVTrimFeatures::parseFeatureBits(STI);
  if (!ISAInfo)
    reportFatalUsageError(ISAInfo.takeError());
  return getTargetABI((*ISAInfo)->computeDefaultABI());
}

ABI getTargetABI(StringRef ABIName) {
  auto TargetABI = StringSwitch<ABI>(ABIName)
                       .Case("ilp32", ABI_ILP32)
                       .Case("ilp32f", ABI_ILP32F)
                       .Case("ilp32d", ABI_ILP32D)
                       .Case("ilp32e", ABI_ILP32E)
                       .Case("il32pc64", ABI_IL32PC64)
                       .Case("il32pc64f", ABI_IL32PC64F)
                       .Case("il32pc64d", ABI_IL32PC64D)
                       .Case("il32pc64e", ABI_IL32PC64E)
                       .Case("lp64", ABI_LP64)
                       .Case("lp64f", ABI_LP64F)
                       .Case("lp64d", ABI_LP64D)
                       .Case("lp64e", ABI_LP64E)
                       .Case("l64pc128", ABI_L64PC128)
                       .Case("l64pc128f", ABI_L64PC128F)
                       .Case("l64pc128d", ABI_L64PC128D)
                       .Case("cheriot", ABI_CHERIOT)
                       .Default(ABI_Unknown);
  return TargetABI;
}

// To avoid the BP value clobbered by a function call, we need to choose a
// callee saved register to save the value. RV32E only has X8 and X9 as callee
// saved registers and X8 will be used as fp. So we choose X9 as bp.
MCRegister getBPReg() { return RVTrim::X9; }

// Returns the register holding shadow call stack pointer.
MCRegister getSCSPReg() { return RVTrim::X3; }

} // namespace RVTrimABI

namespace RVTrimFeatures {

void validate(const Triple &TT, const FeatureBitset &FeatureBits) {
  if (TT.isArch64Bit() && !FeatureBits[RVTrim::Feature64Bit])
    reportFatalUsageError("RV64 target requires an RV64 CPU");
  if (!TT.isArch64Bit() && !FeatureBits[RVTrim::Feature32Bit])
    reportFatalUsageError("RV32 target requires an RV32 CPU");
  if (FeatureBits[RVTrim::Feature32Bit] &&
      FeatureBits[RVTrim::Feature64Bit])
    reportFatalUsageError("RV32 and RV64 can't be combined");
}

RVTrimProfile getProfile(const MCSubtargetInfo &STI) {
  if (STI.getFeatureBits()[RVTrim::FeatureStdExtZve64x])
    return RVTrimProfile::BaseIMA_Zve64x;
  return RVTrimProfile::BaseIMA;
}

static StringRef getProfileName(RVTrimProfile P) {
  switch (P) {
  case RVTrimProfile::BaseIMA:
    return "RV64IMA";
  case RVTrimProfile::BaseIMA_Zve64x:
    return "RV64IMA_Zve64x";
  }
  llvm_unreachable("unknown RVTrim profile");
}

bool isExtensionAllowed(StringRef Ext, RVTrimProfile P) {
  static constexpr StringLiteral BaseIMA[] = {
      "i", "m", "a", "zicsr", "zifencei", "zmmul", "zaamo", "zalrsc"};
  if (llvm::is_contained(BaseIMA, Ext))
    return true;

  if (P != RVTrimProfile::BaseIMA_Zve64x)
    return false;

  // zve64x implies zve32x and zvl64b; zve32x and zvl64b both imply zvl32b.
  // Keep this list in sync with RISCVFeatures.td and the ImpliedExts table in
  // RISCVISAInfo.cpp, which is what populates getExtensions().
  static constexpr StringLiteral Vector[] = {"zve64x", "zve32x", "zvl64b",
                                             "zvl32b"};
  return llvm::is_contained(Vector, Ext);
}

void validateProfile(const MCSubtargetInfo &STI) {
  auto ISAInfoOrErr = parseFeatureBits(STI);
  if (!ISAInfoOrErr) {
    // Extension parsing errors are diagnosed by the regular feature checks.
    consumeError(ISAInfoOrErr.takeError());
    return;
  }

  RVTrimProfile P = getProfile(STI);
  for (const auto &[Ext, Version] : (*ISAInfoOrErr)->getExtensions()) {
    if (isExtensionAllowed(Ext, P))
      continue;
    reportFatalUsageError(
        Twine("RVTrim implements only ") + getProfileName(P) +
        (P == RVTrimProfile::BaseIMA_Zve64x
             ? " (i, m, a, zicsr, zifencei, zve64x); extension '"
             : " (i, m, a, zicsr, zifencei); extension '") +
        Ext + "' is outside the trimmed profile");
  }
}

llvm::Expected<std::unique_ptr<RISCVISAInfo>>
parseFeatureBits(const MCSubtargetInfo &STI) {
  const FeatureBitset &FeatureBits = STI.getFeatureBits();
  unsigned XLen = FeatureBits[RVTrim::Feature64Bit] ? 64 : 32;
  std::vector<std::string> FeatureVector;
  // Convert FeatureBitset to FeatureVector.
  for (const auto &Feature : STI.getAllProcessorFeatures()) {
    if (FeatureBits[Feature.Value] &&
        llvm::RISCVISAInfo::isSupportedExtensionFeature(Feature.key()))
      FeatureVector.push_back(std::string("+") + Feature.key());
  }
  return llvm::RISCVISAInfo::parseFeatures(XLen, FeatureVector);
}

} // namespace RVTrimFeatures

// Include the auto-generated portion of the compress emitter.
#define GEN_UNCOMPRESS_INSTR
#define GEN_COMPRESS_INSTR
#include "RVTrimGenCompressInstEmitter.inc"

bool RVTrimRVC::compress(MCInst &OutInst, const MCInst &MI,
                        const MCSubtargetInfo &STI) {
  return compressInst(OutInst, MI, STI);
}

bool RVTrimRVC::uncompress(MCInst &OutInst, const MCInst &MI,
                          const MCSubtargetInfo &STI) {
  return uncompressInst(OutInst, MI, STI);
}

// Lookup table for fli.s for entries 2-31.
static constexpr std::pair<uint8_t, uint8_t> LoadFP32ImmArr[] = {
    {0b01101111, 0b00}, {0b01110000, 0b00}, {0b01110111, 0b00},
    {0b01111000, 0b00}, {0b01111011, 0b00}, {0b01111100, 0b00},
    {0b01111101, 0b00}, {0b01111101, 0b01}, {0b01111101, 0b10},
    {0b01111101, 0b11}, {0b01111110, 0b00}, {0b01111110, 0b01},
    {0b01111110, 0b10}, {0b01111110, 0b11}, {0b01111111, 0b00},
    {0b01111111, 0b01}, {0b01111111, 0b10}, {0b01111111, 0b11},
    {0b10000000, 0b00}, {0b10000000, 0b01}, {0b10000000, 0b10},
    {0b10000001, 0b00}, {0b10000010, 0b00}, {0b10000011, 0b00},
    {0b10000110, 0b00}, {0b10000111, 0b00}, {0b10001110, 0b00},
    {0b10001111, 0b00}, {0b11111111, 0b00}, {0b11111111, 0b10},
};

int RVTrimLoadFPImm::getLoadFPImm(APFloat FPImm) {
  assert((&FPImm.getSemantics() == &APFloat::IEEEsingle() ||
          &FPImm.getSemantics() == &APFloat::IEEEdouble() ||
          &FPImm.getSemantics() == &APFloat::IEEEhalf()) &&
         "Unexpected semantics");

  // Handle the minimum normalized value which is different for each type.
  if (FPImm.isSmallestNormalized() && !FPImm.isNegative())
    return 1;

  // Convert to single precision to use its lookup table.
  bool LosesInfo;
  APFloat::opStatus Status = FPImm.convert(
      APFloat::IEEEsingle(), APFloat::rmNearestTiesToEven, &LosesInfo);
  if (Status != APFloat::opOK || LosesInfo)
    return -1;

  APInt Imm = FPImm.bitcastToAPInt();

  if (Imm.extractBitsAsZExtValue(21, 0) != 0)
    return -1;

  bool Sign = Imm.extractBitsAsZExtValue(1, 31);
  uint8_t Mantissa = Imm.extractBitsAsZExtValue(2, 21);
  uint8_t Exp = Imm.extractBitsAsZExtValue(8, 23);

  auto EMI = llvm::lower_bound(LoadFP32ImmArr, std::make_pair(Exp, Mantissa));
  if (EMI == std::end(LoadFP32ImmArr) || EMI->first != Exp ||
      EMI->second != Mantissa)
    return -1;

  // Table doesn't have entry 0 or 1.
  int Entry = std::distance(std::begin(LoadFP32ImmArr), EMI) + 2;

  // The only legal negative value is -1.0(entry 0). 1.0 is entry 16.
  if (Sign) {
    if (Entry == 16)
      return 0;
    return -1;
  }

  return Entry;
}

float RVTrimLoadFPImm::getFPImm(unsigned Imm) {
  assert(Imm != 1 && Imm != 30 && Imm != 31 && "Unsupported immediate");

  // Entry 0 is -1.0, the only negative value. Entry 16 is 1.0.
  uint32_t Sign = 0;
  if (Imm == 0) {
    Sign = 0b1;
    Imm = 16;
  }

  uint32_t Exp = LoadFP32ImmArr[Imm - 2].first;
  uint32_t Mantissa = LoadFP32ImmArr[Imm - 2].second;

  uint32_t I = Sign << 31 | Exp << 23 | Mantissa << 21;
  return bit_cast<float>(I);
}

void RVTrimZC::printRegList(unsigned RlistEncode, raw_ostream &OS) {
  assert(RlistEncode >= RLISTENCODE::RA &&
         RlistEncode <= RLISTENCODE::RA_S0_S11 && "Invalid Rlist");
  OS << "{ra";
  if (RlistEncode > RVTrimZC::RA) {
    OS << ", s0";
    if (RlistEncode == RVTrimZC::RA_S0_S11)
      OS << "-s11";
    else if (RlistEncode > RVTrimZC::RA_S0 && RlistEncode <= RVTrimZC::RA_S0_S11)
      OS << "-s" << (RlistEncode - RVTrimZC::RA_S0);
  }
  OS << "}";
}

} // namespace llvm
