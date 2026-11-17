//===-- RVMinAsmPrinter.cpp - RVMin assembly printer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AsmPrinter walks the MachineFunction and hands each instruction to the MC
// layer.  MachineInstr -> MCInst conversion lives in RVMinMCInstLower.cpp.
//
//===----------------------------------------------------------------------===//

#include "RVMinAsmPrinter.h"
#include "RVMin.h"
#include "RVMinBaseInfo.h"
#include "MCTargetDesc/RVMinInstPrinter.h"
#include "RVMinMCInstLower.h"
#include "MCTargetDesc/RVMinMCTargetDesc.h"
#include "RVMinTargetMachine.h"
#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

// Chapter 44: the TableGen-generated compressor.  It defines
//
//   static bool compressInst(MCInst &Out, const MCInst &MI,
//                            const MCSubtargetInfo &STI);
//
// from the CompressPat records in RVMinInstrInfoC.td, including the
// HasStdExtC/F/D feature checks, so it is safe to call unconditionally.
#define GEN_COMPRESS_INSTR
#include "RVMinGenCompressInstEmitter.inc"

namespace {

class RVMinAsmPrinter : public AsmPrinter {
public:
  RVMinAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer), ID) {}

  StringRef getPassName() const override { return "RVMin Assembly Printer"; }

  /// Chapter 22: inline asm operands.  The LL/SC loop created by AtomicExpand
  /// embeds `lr.w.aqrl` / `sc.w.aqrl` as one-instruction inline asm, so the
  /// printer has to render its register operands.
  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &O) override {
    if (!AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, O))
      return false;
    if (ExtraCode && ExtraCode[0])
      return true; // Unknown modifier.
    const MachineOperand &MO = MI->getOperand(OpNo);
    if (MO.isReg()) {
      O << RVMinInstPrinter::getRegisterName(MO.getReg(), /*AltIdx=*/0);
      return false;
    }
    if (MO.isImm()) {
      O << MO.getImm();
      return false;
    }
    return true;
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    SetupMachineFunction(MF);
    emitFunctionBody();
    return false;
  }

  /// Chapter 48: true for the data-processing vector instructions that depend
  /// on VL/VTYPE.  The vset* family is excluded: it configures the state and
  /// must not recursively trigger another configuration.
  bool isVectorDataInstr(unsigned Opc) const {
    if (Opc == RVMin::VSETIVLI || Opc == RVMin::VSETVLI ||
        Opc == RVMin::VSETVL)
      return false;
    const MCInstrDesc &Desc = TM.getMCInstrInfo()->get(Opc);
    return (Desc.TSFlags & 0x1f) == 17; // FmtV
  }

  /// Emit the conservative configuration for the <4 x i32>, e32/m1, ta/ma
  /// subset.  Every data vector instruction is preceded by this sequence;
  /// chapter 49 removes redundant configurations.
  void emitVSETIVLI() {
    MCInst VSet;
    VSet.setOpcode(RVMin::VSETIVLI);
    VSet.addOperand(MCOperand::createReg(RVMin::X0));
    VSet.addOperand(MCOperand::createImm(4));   // VL = 4
    VSet.addOperand(MCOperand::createImm(0xD0)); // e32, m1, ta, ma
    OutStreamer->emitInstruction(VSet, getSubtargetInfo());
  }

  /// Emit one MC instruction, replacing it with its compressed form when the
  /// 16-bit encoding fits.  Every emission path (pseudo expansion, direct
  /// call lowering, real instructions) funnels through here so the .text
  /// layout and the compressor can never disagree.
  void emitMCInst(const MCInst &Inst) {
    if (getSubtargetInfo().hasFeature(RVMin::FeatureStdExtV) &&
        isVectorDataInstr(Inst.getOpcode()))
      emitVSETIVLI();

    MCInst CInst;
    if (compressInst(CInst, Inst, getSubtargetInfo()))
      OutStreamer->emitInstruction(CInst, getSubtargetInfo());
    else
      OutStreamer->emitInstruction(Inst, getSubtargetInfo());
  }

  void emitInstruction(const MachineInstr *MI) override {
    if (MI->getOpcode() == RVMin::PseudoCALL ||
        MI->getOpcode() == RVMin::PseudoTAIL) {
      emitPseudoCall(*MI, /*IsTail=*/MI->getOpcode() == RVMin::PseudoTAIL);
      return;
    }

    if (MI->isPseudo()) {
      MCInst Expanded;
      if (lowerPseudoInstExpansion(MI, Expanded)) {
        emitMCInst(Expanded);
        return;
      }
      llvm_unreachable("RVMin: unimplemented pseudo instruction reached the "
                       "asm printer");
    }

    MCInst TmpInst;
    LowerRVMinMachineInstrToMCInst(MI, TmpInst, OutContext, TM, *this);
    emitMCInst(TmpInst);
  }

  /// TableGen-generated expansion for instructions declared with
  /// PseudoInstExpansion (PseudoRET, PseudoBR and PseudoCALLR).
  bool lowerPseudoInstExpansion(const MachineInstr *MI, MCInst &Inst);

  /// Expand a direct call (or a direct tail call) into the three real
  /// instructions that materialize the callee address and jump to it:
  ///
  ///   lui  t0, %hi(callee)
  ///   addi t0, t0, %lo(callee)
  ///   jalr rd, t0, 0
  ///
  /// A call links with `rd = ra`; a tail call uses `rd = x0`, i.e. `j` -- the
  /// return address is still our caller's, which is the whole point of the
  /// optimization.
  ///
  /// The `%hi`/`%lo` specifiers become R_RISCV_HI20 / R_RISCV_LO12_I
  /// relocations, exactly like a global data reference.
  void emitPseudoCall(const MachineInstr &MI, bool IsTail) {
    const MachineOperand &Callee = MI.getOperand(0);

    MCSymbol *Sym;
    if (Callee.isGlobal())
      Sym = TM.getSymbol(Callee.getGlobal());
    else if (Callee.isSymbol())
      Sym = OutContext.getOrCreateSymbol(Callee.getSymbolName());
    else
      llvm_unreachable("RVMin: direct call without a symbol callee");

    const MCExpr *Base = MCSymbolRefExpr::create(Sym, OutContext);

    MCInst Lui;
    Lui.setOpcode(RVMin::LUI);
    Lui.addOperand(MCOperand::createReg(RVMin::X5));
    Lui.addOperand(MCOperand::createExpr(
        MCSpecifierExpr::create(Base, rvmin::S_HI20, OutContext)));
    emitMCInst(Lui);

    MCInst Addi;
    Addi.setOpcode(RVMin::ADDI);
    Addi.addOperand(MCOperand::createReg(RVMin::X5));
    Addi.addOperand(MCOperand::createReg(RVMin::X5));
    Addi.addOperand(MCOperand::createExpr(
        MCSpecifierExpr::create(Base, rvmin::S_LO12_I, OutContext)));
    emitMCInst(Addi);

    MCInst Jalr;
    Jalr.setOpcode(RVMin::JALR);
    Jalr.addOperand(MCOperand::createReg(IsTail ? RVMin::X0 : RVMin::X1));
    Jalr.addOperand(MCOperand::createReg(RVMin::X5));
    Jalr.addOperand(MCOperand::createImm(0));
    emitMCInst(Jalr);
  }

  /// Operand helper used by the generated pseudo-expansion code: a pseudo
  /// branch carries a basic block, which becomes a symbol reference.
  void lowerOperand(const MachineOperand &MO, MCOperand &MCOp) {
    switch (MO.getType()) {
    case MachineOperand::MO_Register:
      MCOp = MCOperand::createReg(MO.getReg());
      return;
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::createImm(MO.getImm());
      return;
    case MachineOperand::MO_MachineBasicBlock:
      MCOp = MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), OutContext));
      return;
    default:
      llvm_unreachable("RVMin: unsupported pseudo-instruction operand");
    }
  }

  static char ID;
};

char RVMinAsmPrinter::ID = 0;

#include "RVMinGenMCPseudoLowering.inc"

} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinAsmPrinter() {
  RegisterAsmPrinter<RVMinAsmPrinter> X(getTheRVMinTarget());
}

PreservedAnalyses RVMinAsmPrinterBeginPass::run(Module &M,
                                                ModuleAnalysisManager &MAM) {
  RVMinAsmPrinter &AsmPrinter = static_cast<RVMinAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
RVMinAsmPrinterPass::run(MachineFunction &MF,
                         MachineFunctionAnalysisManager &MFAM) {
  RVMinAsmPrinter &AsmPrinter = static_cast<RVMinAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses RVMinAsmPrinterEndPass::run(Module &M,
                                              ModuleAnalysisManager &MAM) {
  RVMinAsmPrinter &AsmPrinter = static_cast<RVMinAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
