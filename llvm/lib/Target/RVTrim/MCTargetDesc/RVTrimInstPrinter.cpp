//===-- RVTrimInstPrinter.cpp - Convert RISC-V MCInst to asm syntax --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This class prints an RISC-V MCInst to a .s file.
//
//===----------------------------------------------------------------------===//

#include "RVTrimInstPrinter.h"
#include "RVTrimBaseInfo.h"
#include "RVTrimMCAsmInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

// Include the auto-generated portion of the assembly writer.
#define PRINT_ALIAS_INSTR
#include "RVTrimGenAsmWriter.inc"

// Print architectural register names rather than the ABI names (such as x2
// instead of sp).
// TODO: Make RVTrimInstPrinter::getRegisterName non-static so that this and
// EmitX8AsFP can be members.
static bool ArchRegNames;
static bool EmitX8AsFP;

RVTrimInstPrinter::RVTrimInstPrinter(const MCAsmInfo &MAI, const MCInstrInfo &MII,
                                   const MCRegisterInfo &MRI)
    : MCInstPrinter(MAI, MII, MRI),
      CLOpts(RVTrim_MC::getCLOpts(MAI.getTargetOptions())) {}

// -rvtrim-no-aliases and -rvtrim-emit-x8-as-fp are used by llvm-mc and llc. They
// can be used by `llvm-objdump`, but we override their values here to handle
// options passed to `llvm-objdump` with `-M` (which matches GNU objdump). There
// did not seem to be an easier way to allow these options in all these tools,
// without doing it this way.
bool RVTrimInstPrinter::applyTargetSpecificCLOption(StringRef Opt) {
  if (Opt == "no-aliases") {
    PrintAliases = false;
    return true;
  }
  if (Opt == "numeric") {
    ArchRegNames = true;
    return true;
  }
  if (Opt == "emit-x8-as-fp") {
    if (!ArchRegNames)
      EmitX8AsFP = true;
    return true;
  }

  return false;
}

void RVTrimInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                 StringRef Annot, const MCSubtargetInfo &STI,
                                 raw_ostream &O) {
  bool Res = false;
  const MCInst *NewMI = MI;
  MCInst UncompressedMI;
  if (PrintAliases && !CLOpts.no_aliases)
    Res = RVTrimRVC::uncompress(UncompressedMI, *MI, STI);
  if (Res)
    NewMI = &UncompressedMI;
  if (!PrintAliases || CLOpts.no_aliases ||
      !printAliasInstr(NewMI, Address, STI, O))
    printInstruction(NewMI, Address, STI, O);
  printAnnotation(O, Annot);
}

void RVTrimInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  markup(O, Markup::Register) << getRegisterName(CLOpts, Reg);
}

void RVTrimInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                    const MCSubtargetInfo &STI,
                                    raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  if (MO.isReg()) {
    printRegName(O, MO.getReg());
    return;
  }

  if (MO.isImm()) {
    printImm(MI, OpNo, STI, O);
    return;
  }

  assert(MO.isExpr() && "Unknown operand kind in printOperand");
  MAI.printExpr(O, *MO.getExpr());
}

void RVTrimInstPrinter::printBranchOperand(const MCInst *MI, uint64_t Address,
                                          unsigned OpNo,
                                          const MCSubtargetInfo &STI,
                                          raw_ostream &O) {
  // Do not print the numeric target address when symbolizing.
  if (SymbolizeOperands)
    return;

  const MCOperand &MO = MI->getOperand(OpNo);
  if (!MO.isImm())
    return printOperand(MI, OpNo, STI, O);

  if (PrintBranchImmAsAddress) {
    uint64_t Target = Address + MO.getImm();
    if (!STI.hasFeature(RVTrim::Feature64Bit))
      Target &= 0xffffffff;
    markup(O, Markup::Target) << formatHex(Target);
  } else {
    markup(O, Markup::Target) << formatImm(MO.getImm());
  }
}

void RVTrimInstPrinter::printCSRSystemRegister(const MCInst *MI, unsigned OpNo,
                                              const MCSubtargetInfo &STI,
                                              raw_ostream &O) {
  unsigned Imm = MI->getOperand(OpNo).getImm();
  auto Range = RVTrimSysReg::lookupSysRegByEncoding(Imm);
  bool PreferFeatureSpecific = llvm::any_of(Range, [&](const auto &Reg) {
    return !Reg.IsAltName && !Reg.IsDeprecatedName &&
           Reg.FeaturesRequired.any() &&
           Reg.haveRequiredFeatures(STI.getFeatureBits());
  });
  for (auto &Reg : Range) {
    if (Reg.IsAltName || Reg.IsDeprecatedName)
      continue;
    if (Reg.haveRequiredFeatures(STI.getFeatureBits()) &&
        (!PreferFeatureSpecific || Reg.FeaturesRequired.any())) {
      markup(O, Markup::Register) << RVTrimSysReg::getSysRegStr(Reg.Name);
      return;
    }
  }
  markup(O, Markup::Register) << formatImm(Imm);
}

void RVTrimInstPrinter::printFenceArg(const MCInst *MI, unsigned OpNo,
                                     const MCSubtargetInfo &STI,
                                     raw_ostream &O) {
  unsigned FenceArg = MI->getOperand(OpNo).getImm();
  assert (((FenceArg >> 4) == 0) && "Invalid immediate in printFenceArg");

  if ((FenceArg & RVTrimFenceField::I) != 0)
    O << 'i';
  if ((FenceArg & RVTrimFenceField::O) != 0)
    O << 'o';
  if ((FenceArg & RVTrimFenceField::R) != 0)
    O << 'r';
  if ((FenceArg & RVTrimFenceField::W) != 0)
    O << 'w';
  if (FenceArg == 0)
    O << "0";
}

void RVTrimInstPrinter::printSMTVType(const MCInst *MI, unsigned OpNo,
                                     const MCSubtargetInfo &STI,
                                     raw_ostream &O) {
  auto VType =
      static_cast<XSMTVTypeMode::SMTVTypeMode>(MI->getOperand(OpNo).getImm());
  assert(XSMTVTypeMode::isValidSMTVTypeMode(VType) &&
         "SpacemiT's Integer Matrix only supports [i4|i8] mode");
  O << ", " << XSMTVTypeMode::SMTVTypeModeToString(VType);
}

void RVTrimInstPrinter::printFRMArg(const MCInst *MI, unsigned OpNo,
                                   const MCSubtargetInfo &STI, raw_ostream &O) {
  auto FRMArg =
      static_cast<RVTrimFPRndMode::RoundingMode>(MI->getOperand(OpNo).getImm());
  if (PrintAliases && !CLOpts.no_aliases &&
      FRMArg == RVTrimFPRndMode::RoundingMode::DYN)
    return;
  O << ", " << RVTrimFPRndMode::roundingModeToString(FRMArg);
}

void RVTrimInstPrinter::printFRMArgLegacy(const MCInst *MI, unsigned OpNo,
                                         const MCSubtargetInfo &STI,
                                         raw_ostream &O) {
  auto FRMArg =
      static_cast<RVTrimFPRndMode::RoundingMode>(MI->getOperand(OpNo).getImm());
  // Never print rounding mode if it's the default 'rne'. This ensures the
  // output can still be parsed by older tools that erroneously failed to
  // accept a rounding mode.
  if (FRMArg == RVTrimFPRndMode::RoundingMode::RNE)
    return;
  O << ", " << RVTrimFPRndMode::roundingModeToString(FRMArg);
}

void RVTrimInstPrinter::printFPImmOperand(const MCInst *MI, unsigned OpNo,
                                         const MCSubtargetInfo &STI,
                                         raw_ostream &O) {
  unsigned Imm = MI->getOperand(OpNo).getImm();
  if (Imm == 1) {
    markup(O, Markup::Immediate) << "min";
  } else if (Imm == 30) {
    markup(O, Markup::Immediate) << "inf";
  } else if (Imm == 31) {
    markup(O, Markup::Immediate) << "nan";
  } else {
    float FPVal = RVTrimLoadFPImm::getFPImm(Imm);
    // If the value is an integer, print a .0 fraction. Otherwise, use %g to
    // which will not print trailing zeros and will use scientific notation
    // if it is shorter than printing as a decimal. The smallest value requires
    // 12 digits of precision including the decimal.
    if (FPVal == (int)(FPVal))
      markup(O, Markup::Immediate) << format("%.1f", FPVal);
    else
      markup(O, Markup::Immediate) << format("%.12g", FPVal);
  }
}

void RVTrimInstPrinter::printZeroOffsetMemOp(const MCInst *MI, unsigned OpNo,
                                            const MCSubtargetInfo &STI,
                                            raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  assert(MO.isReg() && "printZeroOffsetMemOp can only print register operands");
  O << "(";
  printRegName(O, MO.getReg());
  O << ")";
}

void RVTrimInstPrinter::printVTypeI(const MCInst *MI, unsigned OpNo,
                                   const MCSubtargetInfo &STI, raw_ostream &O) {
  unsigned Imm = MI->getOperand(OpNo).getImm();
  // Print the raw immediate for reserved values: vlmul[2:0]=4, vsew[2:0]=0b1xx,
  // altfmt=1 without sew=8 or sew=16, or non-zero in bits 9 and above.
  if (!RISCVVType::isValidVType(Imm) || (Imm >> 9) != 0) {
    O << formatImm(Imm);
    return;
  }
  // Print the text form.
  RISCVVType::printVType(Imm, O);
}

void RVTrimInstPrinter::printXSfmmVType(const MCInst *MI, unsigned OpNo,
                                       const MCSubtargetInfo &STI,
                                       raw_ostream &O) {
  unsigned Imm = MI->getOperand(OpNo).getImm();
  assert(RISCVVType::isValidXSfmmVType(Imm));
  unsigned SEW = RISCVVType::getSEW(Imm);
  O << "e" << SEW;
  bool AltFmt = RISCVVType::isAltFmt(Imm);
  if (AltFmt)
    O << "alt";
  unsigned Widen = RISCVVType::getXSfmmWiden(Imm);
  O << ", w" << Widen;
}

// Print a Zcmp RList. If we are printing architectural register names rather
// than ABI register names, we need to print "{x1, x8-x9, x18-x27}" for all
// registers. Otherwise, we print "{ra, s0-s11}".
void RVTrimInstPrinter::printRegList(const MCInst *MI, unsigned OpNo,
                                    const MCSubtargetInfo &STI, raw_ostream &O) {
  unsigned Imm = MI->getOperand(OpNo).getImm();

  assert(Imm >= RVTrimZC::RLISTENCODE::RA &&
         Imm <= RVTrimZC::RLISTENCODE::RA_S0_S11 && "Invalid Rlist");

  O << "{";
  printRegName(O, RVTrim::X1);

  if (Imm >= RVTrimZC::RLISTENCODE::RA_S0) {
    O << ", ";
    printRegName(O, RVTrim::X8);
  }

  if (Imm >= RVTrimZC::RLISTENCODE::RA_S0_S1) {
    O << '-';
    if (Imm == RVTrimZC::RLISTENCODE::RA_S0_S1 || ArchRegNames)
      printRegName(O, RVTrim::X9);
  }

  if (Imm >= RVTrimZC::RLISTENCODE::RA_S0_S2) {
    if (ArchRegNames)
      O << ", ";
    if (Imm == RVTrimZC::RLISTENCODE::RA_S0_S2 || ArchRegNames)
      printRegName(O, RVTrim::X18);
  }

  if (Imm >= RVTrimZC::RLISTENCODE::RA_S0_S3) {
    if (ArchRegNames)
      O << '-';
    unsigned Offset = (Imm - RVTrimZC::RLISTENCODE::RA_S0_S3);
    // Encodings for S3-S9 are contiguous. There is no encoding for S10, so we
    // must skip to S11(X27).
    if (Imm == RVTrimZC::RLISTENCODE::RA_S0_S11)
      ++Offset;
    printRegName(O, RVTrim::X19 + Offset);
  }

  O << "}";
}

void RVTrimInstPrinter::printRegReg(const MCInst *MI, unsigned OpNo,
                                   const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &OffsetMO = MI->getOperand(OpNo + 1);

  assert(OffsetMO.isReg() && "printRegReg can only print register operands");
  printRegName(O, OffsetMO.getReg());

  O << "(";
  const MCOperand &BaseMO = MI->getOperand(OpNo);
  assert(BaseMO.isReg() && "printRegReg can only print register operands");
  printRegName(O, BaseMO.getReg());
  O << ")";
}

void RVTrimInstPrinter::printStackAdj(const MCInst *MI, unsigned OpNo,
                                     const MCSubtargetInfo &STI, raw_ostream &O,
                                     bool Negate) {
  int64_t Imm = MI->getOperand(OpNo).getImm();
  bool IsRV64 = STI.hasFeature(RVTrim::Feature64Bit);
  int64_t StackAdj = 0;
  auto RlistVal = MI->getOperand(0).getImm();
  auto Base = RVTrimZC::getStackAdjBase(RlistVal, IsRV64);
  StackAdj = Imm + Base;
  assert((StackAdj >= Base && StackAdj <= Base + 48) &&
         "Incorrect stack adjust");
  if (Negate)
    StackAdj = -StackAdj;

  // RAII guard for ANSI color escape sequences
  WithMarkup ScopedMarkup = markup(O, Markup::Immediate);
  O << StackAdj;
}

void RVTrimInstPrinter::printVMaskReg(const MCInst *MI, unsigned OpNo,
                                     const MCSubtargetInfo &STI,
                                     raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  assert(MO.isReg() && "printVMaskReg can only print register operands");
  if (MO.getReg() == RVTrim::NoRegister)
    return;
  O << ", ";
  printRegName(O, MO.getReg());
  O << ".t";
}

void RVTrimInstPrinter::printVScaleReg(const MCInst *MI, unsigned OpNo,
                                      const MCSubtargetInfo &STI,
                                      raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  assert(MO.isReg() && "printVScaleReg can only print register operands");
  O << ", ";
  printRegName(O, MO.getReg());
  O << ".scale";
}

void RVTrimInstPrinter::printTileLambda(const MCInst *MI, unsigned OpNo,
                                       const MCSubtargetInfo &STI,
                                       raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  assert(MO.isImm() && "printTileLambda can only print immediate operands");
  unsigned Lambda = MO.getImm();
  if (Lambda == 0)
    return;

  assert(Lambda <= 7 && "Unexpected tile lambda encoding");
  O << ", L" << (1U << (Lambda - 1));
}

void RVTrimInstPrinter::printImm(const MCInst *MI, unsigned OpNo,
                                const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  const unsigned Opcode = MI->getOpcode();
  uint64_t Imm = Op.getImm();
  if (STI.getTargetTriple().isOSBinFormatMachO() &&
      (Opcode == RVTrim::ANDI || Opcode == RVTrim::ORI || Opcode == RVTrim::XORI ||
       Opcode == RVTrim::C_ANDI || Opcode == RVTrim::AUIPC ||
       Opcode == RVTrim::LUI)) {
    if (!STI.hasFeature(RVTrim::Feature64Bit))
      Imm &= 0xffffffff;
    markup(O, Markup::Immediate) << formatHex(Imm);
  } else
    markup(O, Markup::Immediate) << formatImm(Imm);
}

const char *RVTrimInstPrinter::getRegisterName(const RVTrimMCOptions &CLOpts,
                                              MCRegister Reg) {
  // When PrintAliases is enabled, and EmitX8AsFP is enabled, x8 will be printed
  // as fp instead of s0. Note that these similar registers are not replaced:
  // - X8_H: used for f16 register in zhinx
  // - X8_W: used for f32 register in zfinx
  // - X8_X9: used for GPR Pair
  if (!ArchRegNames && (EmitX8AsFP || CLOpts.emit_x8_as_fp) && Reg == RVTrim::X8)
    return "fp";
  return getRegisterName(Reg, ArchRegNames ? RVTrim::NoRegAltName
                                           : RVTrim::ABIRegAltName);
}
