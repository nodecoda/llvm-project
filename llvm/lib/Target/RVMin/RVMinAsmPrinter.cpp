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
    VStateValid = false;
    emitFunctionBody();
    return false;
  }

  /// Chapter 49: VL/VTYPE are architectural state.  A new basic block can be
  /// reached from an arbitrary predecessor (or a call), so forget the cached
  /// configuration at every block boundary.  No cross-block analysis.
  void emitBasicBlockStart(const MachineBasicBlock &MBB) override {
    VStateValid = false;
    AsmPrinter::emitBasicBlockStart(MBB);
  }

  /// Chapter 48: true for the data-processing vector instructions that depend
  /// on VL/VTYPE.  The vset* family is excluded: it configures the state and
  /// must not recursively trigger another configuration.
  bool isVectorDataInstr(unsigned Opc) const {
    if (Opc == RVMin::VSETIVLI || Opc == RVMin::VSETVLI ||
        Opc == RVMin::VSETVL)
      return false;
    // Chapter 53: whole-register moves ignore VL/VTYPE.
    if (Opc == RVMin::VMV2R_V)
      return false;
    const MCInstrDesc &Desc = TM.getMCInstrInfo()->get(Opc);
    return (Desc.TSFlags & 0x1f) == 17; // FmtV
  }

  /// Emit the configuration for the <4 x i32>, e32/m1, ta/ma subset.  Chapter
  /// 48 emitted it before every data instruction; chapter 49 caches it and
  /// re-emits only at block boundaries, calls and explicit vset instructions.
  void emitVSETIVLI(unsigned VL, unsigned VT) {
    MCInst VSet;
    VSet.setOpcode(RVMin::VSETIVLI);
    VSet.addOperand(MCOperand::createReg(RVMin::X0));
    VSet.addOperand(MCOperand::createImm(VL));
    VSet.addOperand(MCOperand::createImm(VT));
    OutStreamer->emitInstruction(VSet, getSubtargetInfo());
  }

  /// Emit one MC instruction, replacing it with its compressed form when the
  /// 16-bit encoding fits.  Every emission path (pseudo expansion, direct
  /// call lowering, real instructions) funnels through here so the .text
  /// layout and the compressor can never disagree.
  void emitMCInst(const MCInst &Inst) {
    if (getSubtargetInfo().hasFeature(RVMin::FeatureStdExtV)) {
      unsigned Opc = Inst.getOpcode();
      if (Opc == RVMin::VSETIVLI || Opc == RVMin::VSETVLI ||
          Opc == RVMin::VSETVL) {
        // An explicit configuration invalidates our cache; the next vector
        // instruction must re-establish the subset's VL/VTYPE.
        VStateValid = false;
      } else if (isVectorDataInstr(Opc)) {
        // Chapter 53: LMUL=1 uses VL=4/e32,m1 (0xD0); LMUL=2 uses
        // VL=8/e32,m2 (0xD1).  A block can mix the two, so cache both.
        unsigned VL = 4, VT = 0xD0;
        for (const MCOperand &MO : Inst)
          if (MO.isReg() && RVMin::VRM2RegClass.contains(MO.getReg())) {
            VL = 8;
            VT = 0xD1;
            break;
          }
        if (!VStateValid || VStateVL != VL || VStateVT != VT) {
          emitVSETIVLI(VL, VT);
          VStateValid = true;
          VStateVL = VL;
          VStateVT = VT;
        }
      }
      // Calls and returns can clobber VL/VTYPE.  JAL/JALR are also the last
      // instruction of a block; resetting here is cheaper than inspecting
      // every pseudo call.
      if (Opc == RVMin::JAL || Opc == RVMin::JALR)
        VStateValid = false;
    }

    MCInst CInst;
    if (compressInst(CInst, Inst, getSubtargetInfo()))
      OutStreamer->emitInstruction(CInst, getSubtargetInfo());
    else
      OutStreamer->emitInstruction(Inst, getSubtargetInfo());
  }

  /// Chapter 53: LMUL=2 pseudos share their 32-bit encoding with the m1
  /// instruction, so they cannot be separate MC opcodes.  Copy the register
  /// operands onto the real opcode; emitMCInst then sees the 256-bit pair and
  /// emits the m2 vsetivli.  This is the teaching-backend alternative to
  /// upstream's table-driven pseudo lowering, which only supports operands
  /// whose register classes match exactly.
  void emitM2Pseudo(const MachineInstr &MI, unsigned RealOpc) {
    MCInst Inst;
    Inst.setOpcode(RealOpc);
    for (const MachineOperand &MO : MI.operands())
      if (MO.isReg())
        Inst.addOperand(MCOperand::createReg(MO.getReg()));
    emitMCInst(Inst);
  }

  void emitInstruction(const MachineInstr *MI) override {
    switch (MI->getOpcode()) {
    case RVMin::VLE32_V_M2:
      emitM2Pseudo(*MI, RVMin::VLE32_V);
      return;
    case RVMin::VSE32_V_M2:
      emitM2Pseudo(*MI, RVMin::VSE32_V);
      return;
    case RVMin::VADD_VV_M2:
      emitM2Pseudo(*MI, RVMin::VADD_VV);
      return;
    case RVMin::VMV2R_V_M2:
      emitM2Pseudo(*MI, RVMin::VMV2R_V);
      return;
    default:
      break;
    }

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

  /// True when the current basic block already has a valid configuration.
  /// This is assembly-printer state only; the machine-level verifier cannot
  /// see the MC instruction inserted in emitMCInst.
  bool VStateValid = false;
  unsigned VStateVL = 0;
  unsigned VStateVT = 0;

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
