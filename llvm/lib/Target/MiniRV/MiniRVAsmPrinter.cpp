//===-- MiniRVAsmPrinter.cpp - MiniRV LLVM assembly writer ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVAsmPrinter.h"
#include "MCTargetDesc/MiniRVInstPrinter.h"
#include "MiniRVMCInstLower.h"
#include "MiniRVTargetMachine.h"
#include "TargetInfo/MiniRVTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {

class MiniRVAsmPrinter : public AsmPrinter {
public:
  MiniRVAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "MiniRV Assembly Printer"; }

  bool runOnMachineFunction(MachineFunction &MF) override;
  void emitInstruction(const MachineInstr *MI) override;

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &O) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &O) override;

  // Wrapper needed for the tablegenned pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const;
  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  void emitSymbolAddress(const MachineInstr &MI);

  void printOperand(const MachineInstr *MI, unsigned OpNo, raw_ostream &O);

  static char ID;
};

} // end anonymous namespace

char MiniRVAsmPrinter::ID = 0;

bool MiniRVAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  SetupMachineFunction(MF);
  emitFunctionBody();
  return false;
}

bool MiniRVAsmPrinter::lowerOperand(const MachineOperand &MO,
                                    MCOperand &MCOp) const {
  switch (MO.getType()) {
  default:
    return false;
  case MachineOperand::MO_Register:
    if (MO.isImplicit())
      return false;
    MCOp = MCOperand::createReg(MO.getReg());
    return true;
  case MachineOperand::MO_Immediate:
    MCOp = MCOperand::createImm(MO.getImm());
    return true;
  case MachineOperand::MO_MachineBasicBlock:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext));
    return true;
  case MachineOperand::MO_GlobalAddress:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(getSymbol(MO.getGlobal()), OutContext));
    return true;
  case MachineOperand::MO_ExternalSymbol:
    MCOp = MCOperand::createExpr(MCSymbolRefExpr::create(
        GetExternalSymbolSymbol(MO.getSymbolName()), OutContext));
    return true;
  case MachineOperand::MO_MCSymbol:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMCSymbol(), OutContext));
    return true;
  }
}

/// Print one machine operand the way it appears in an ordinary instruction.
/// Inline asm operands are machine operands too, but the generic printer only
/// knows the operand modifiers, not the target's register names.
void MiniRVAsmPrinter::printOperand(const MachineInstr *MI, unsigned OpNo,
                                    raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    O << MiniRVInstPrinter::getRegisterName(MO.getReg());
    return;
  case MachineOperand::MO_Immediate:
    O << MO.getImm();
    return;
  case MachineOperand::MO_MachineBasicBlock:
    O << *MO.getMBB()->getSymbol();
    return;
  case MachineOperand::MO_GlobalAddress:
    O << *getSymbol(MO.getGlobal());
    return;
  case MachineOperand::MO_ExternalSymbol:
    O << *GetExternalSymbolSymbol(MO.getSymbolName());
    return;
  case MachineOperand::MO_MCSymbol:
    O << *MO.getMCSymbol();
    return;
  default:
    llvm_unreachable("Unsupported operand in inline asm");
  }
}

/// A plain operand is printed like an ordinary instruction operand; the
/// modifiers the generic printer understands (c, n, ...) keep working.
bool MiniRVAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                       const char *ExtraCode,
                                       raw_ostream &O) {
  if (!AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, O))
    return false;
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.

  printOperand(MI, OpNo, O);
  return false;
}

/// The memory operand is a base register followed by a displacement, the same
/// pair the address selection produces for an ordinary load or store.
bool MiniRVAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                             unsigned OpNo,
                                             const char *ExtraCode,
                                             raw_ostream &O) {
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.

  const MachineOperand &Base = MI->getOperand(OpNo);
  const MachineOperand &Disp = MI->getOperand(OpNo + 1);
  if (!Base.isReg() || !Disp.isImm())
    return true;

  O << Disp.getImm() << '('
    << MiniRVInstPrinter::getRegisterName(Base.getReg()) << ')';
  return false;
}

/// Emit "auipc rd, %pcrel_hi(sym)" followed by "addi rd, rd, %pcrel_lo(L)".
/// The label L marks the auipc so that the assembler and the linker can find
/// the high part that the low part belongs to.
void MiniRVAsmPrinter::emitSymbolAddress(const MachineInstr &MI) {
  Register Dst = MI.getOperand(0).getReg();
  const MachineOperand &MO = MI.getOperand(1);

  const MCExpr *Target;
  if (MO.isGlobal()) {
    Target = MCSymbolRefExpr::create(getSymbol(MO.getGlobal()), OutContext);
    // A global address can carry a constant offset (a memcpy split into
    // pieces addresses the tail of a constant with one).  Fold it into the
    // expression so that the relocation sees sym+offset.
    if (MO.getOffset())
      Target = MCBinaryExpr::createAdd(
          Target, MCConstantExpr::create(MO.getOffset(), OutContext),
          OutContext);
  } else if (MO.isJTI())
    Target = MCSymbolRefExpr::create(GetJTISymbol(MO.getIndex()), OutContext);
  else
    Target = MCSymbolRefExpr::create(
        GetExternalSymbolSymbol(MO.getSymbolName()), OutContext);

  MCSymbol *HiLabel = OutContext.createTempSymbol("pcrel_hi", true);
  OutStreamer->emitLabel(HiLabel);

  MCInst AUIPC;
  AUIPC.setOpcode(MiniRV::AUIPC);
  AUIPC.addOperand(MCOperand::createReg(Dst));
  AUIPC.addOperand(MCOperand::createExpr(Target));
  EmitToStreamer(*OutStreamer, AUIPC);

  MCInst ADDI;
  ADDI.setOpcode(MiniRV::ADDI);
  ADDI.addOperand(MCOperand::createReg(Dst));
  ADDI.addOperand(MCOperand::createReg(Dst));
  ADDI.addOperand(
      MCOperand::createExpr(MCSymbolRefExpr::create(HiLabel, OutContext)));
  EmitToStreamer(*OutStreamer, ADDI);
}

// PseudoRET and PseudoBR are lowered here, from the records that carry a
// PseudoInstExpansion in MiniRVInstrInfo.td.
#include "MiniRVGenMCPseudoLowering.inc"

void MiniRVAsmPrinter::emitInstruction(const MachineInstr *MI) {
  // Stack maps and patchpoints need a target-specific lowering that records
  // the map and pads the code; the pseudo has no encoding, so reject it
  // instead of letting the instruction printer assert.
  if (MI->getOpcode() == TargetOpcode::STACKMAP ||
      MI->getOpcode() == TargetOpcode::PATCHPOINT)
    report_fatal_error("MiniRV does not support stack maps");

  if (MI->getOpcode() == MiniRV::PseudoLA) {
    emitSymbolAddress(*MI);
    return;
  }

  if (MCInst Expanded; lowerPseudoInstExpansion(MI, Expanded)) {
    EmitToStreamer(*OutStreamer, Expanded);
    return;
  }

  MiniRVMCInstLower MCInstLowering(OutContext, *this);
  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMiniRVAsmPrinter() {
  RegisterAsmPrinter<MiniRVAsmPrinter> X(getTheMiniRVTarget());
}

PreservedAnalyses MiniRVAsmPrinterBeginPass::run(Module &M,
                                                 ModuleAnalysisManager &MAM) {
  MiniRVAsmPrinter &AsmPrinter = static_cast<MiniRVAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
MiniRVAsmPrinterPass::run(MachineFunction &MF,
                          MachineFunctionAnalysisManager &MFAM) {
  MiniRVAsmPrinter &AsmPrinter = static_cast<MiniRVAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses MiniRVAsmPrinterEndPass::run(Module &M,
                                               ModuleAnalysisManager &MAM) {
  MiniRVAsmPrinter &AsmPrinter = static_cast<MiniRVAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
