//===-- RVTrimMCAsmInfo.cpp - RISC-V Asm properties ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declarations of the RVTrimMCAsmInfo properties.
//
//===----------------------------------------------------------------------===//

#include "RVTrimMCAsmInfo.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/TargetParser/Triple.h"
using namespace llvm;

void RVTrimMCAsmInfo::anchor() {}

RVTrimMCAsmInfo::RVTrimMCAsmInfo(const Triple &TT, const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  IsLittleEndian = TT.isLittleEndian();
  CodePointerSize = CalleeSaveStackSlotSize = TT.isArch64Bit() ? 8 : 4;
  CommentString = "#";
  AlignmentIsInBytes = false;
  SupportsDebugInformation = true;
  Data16bitsDirective = "\t.half\t";
  Data32bitsDirective = "\t.word\t";
  // The default symbol subtraction results in an ADD/SUB relocation pair.
  // Processing this relocation pair is problematic when linker relaxation is
  // enabled, so we follow binutils in using the R_RISCV_32_PCREL relocation
  // for the FDE initial location.
  DwarfFDERelSymbolSpec = ELF::R_RISCV_32_PCREL;
}

void RVTrimMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                        const MCSpecifierExpr &Expr) const {
  auto S = Expr.getSpecifier();
  bool HasSpecifier = S != RVTrim::S_None && S != RVTrim::S_CALL_PLT;
  if (HasSpecifier)
    OS << '%' << RVTrim::getSpecifierName(S) << '(';
  printExpr(OS, *Expr.getSubExpr());
  if (HasSpecifier)
    OS << ')';
}

RVTrimMCAsmInfoDarwin::RVTrimMCAsmInfoDarwin(const MCTargetOptions &Options)
    : MCAsmInfoDarwin(Options) {
  CodePointerSize = 4;
  InternalSymbolPrefix = "L";
  SeparatorString = "%%";
  CommentString = ";";
  AlignmentIsInBytes = false;
  SupportsDebugInformation = true;
  UseDataRegionDirectives = true;
  Data16bitsDirective = "\t.half\t";
  Data32bitsDirective = "\t.word\t";
}

void RVTrimMCAsmInfoDarwin::printSpecifierExpr(
    raw_ostream &OS, const MCSpecifierExpr &Expr) const {
  auto S = Expr.getSpecifier();
  bool HasSpecifier = S != RVTrim::S_None && S != RVTrim::S_CALL_PLT;
  if (HasSpecifier)
    OS << '%' << RVTrim::getSpecifierName(S) << '(';
  printExpr(OS, *Expr.getSubExpr());
  if (HasSpecifier)
    OS << ')';
}
