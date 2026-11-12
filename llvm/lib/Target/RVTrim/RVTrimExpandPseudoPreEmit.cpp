//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains one of the four passes that expand pseudo instructions
// into target instructions. This pass is run very late, but before atomic
// instructions are expanded.
//
//===----------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimExpandPseudoBase.h"
#include "RVTrimInstrInfo.h"
#include "RVTrimSubtarget.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define RVTrim_EXPAND_PSEUDO_PRE_EMIT_NAME                                      \
  "RISC-V Pseudo Instruction Expansion - Pre-Emit"

namespace {

class RVTrimExpandPseudoPreEmitImpl final : public RVTrimExpandPseudoImplBase {
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI) const override;

  bool expandCCOp(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                  MachineBasicBlock::iterator &NextMBBI) const;

  bool expandCCOpToCMov(MachineBasicBlock &MBB,
                        MachineBasicBlock::iterator MBBI) const;

  bool expandVMSET_VMCLR(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI,
                         unsigned Opcode) const;

  bool expandMV_FPR16INX(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI) const;

  bool expandMV_FPR32INX(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI) const;

  bool expandRV32ZdinxStore(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MBBI) const;

  bool expandRV32ZdinxLoad(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI) const;

  bool
  expandPseudoReadVLENBViaVSETVLIX0(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator MBBI) const;

  bool expandPseudoClearFPR64(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI) const;
};

class RVTrimExpandPseudoPreEmitLegacy : public MachineFunctionPass {
public:
  static char ID;

  RVTrimExpandPseudoPreEmitLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override {
    return RVTrimExpandPseudoPreEmitImpl().run(MF);
  }

  StringRef getPassName() const override {
    return RVTrim_EXPAND_PSEUDO_PRE_EMIT_NAME;
  }
};

} // anonymous namespace

bool RVTrimExpandPseudoPreEmitImpl::expandMI(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) const {
  // RVTrimInstrInfo::getInstSizeInBytes expects that the total size of the
  // expanded instructions for each pseudo is correct in the Size field of the
  // tablegen definition for the pseudo.
  switch (MBBI->getOpcode()) {
  case RVTrim::PseudoMV_FPR16INX:
    return expandMV_FPR16INX(MBB, MBBI);
  case RVTrim::PseudoMV_FPR32INX:
    return expandMV_FPR32INX(MBB, MBBI);
  case RVTrim::PseudoRV32ZdinxSD:
    return expandRV32ZdinxStore(MBB, MBBI);
  case RVTrim::PseudoRV32ZdinxLD:
    return expandRV32ZdinxLoad(MBB, MBBI);
  case RVTrim::PseudoCCMOVGPRNoX0:
  case RVTrim::PseudoCCMOVGPR:
  case RVTrim::PseudoCCADD:
  case RVTrim::PseudoCCSUB:
  case RVTrim::PseudoCCAND:
  case RVTrim::PseudoCCOR:
  case RVTrim::PseudoCCXOR:
  case RVTrim::PseudoCCMAX:
  case RVTrim::PseudoCCMAXU:
  case RVTrim::PseudoCCMIN:
  case RVTrim::PseudoCCMINU:
  case RVTrim::PseudoCCMUL:
  case RVTrim::PseudoCCLUI:
  case RVTrim::PseudoCCQC_E_LB:
  case RVTrim::PseudoCCQC_E_LH:
  case RVTrim::PseudoCCQC_E_LW:
  case RVTrim::PseudoCCQC_E_LHU:
  case RVTrim::PseudoCCQC_E_LBU:
  case RVTrim::PseudoCCLB:
  case RVTrim::PseudoCCLH:
  case RVTrim::PseudoCCLW:
  case RVTrim::PseudoCCLHU:
  case RVTrim::PseudoCCLBU:
  case RVTrim::PseudoCCLWU:
  case RVTrim::PseudoCCLD:
  case RVTrim::PseudoCCQC_LI:
  case RVTrim::PseudoCCQC_E_LI:
  case RVTrim::PseudoCCADDW:
  case RVTrim::PseudoCCSUBW:
  case RVTrim::PseudoCCSLL:
  case RVTrim::PseudoCCSRL:
  case RVTrim::PseudoCCSRA:
  case RVTrim::PseudoCCADDI:
  case RVTrim::PseudoCCSLLI:
  case RVTrim::PseudoCCSRLI:
  case RVTrim::PseudoCCSRAI:
  case RVTrim::PseudoCCANDI:
  case RVTrim::PseudoCCORI:
  case RVTrim::PseudoCCXORI:
  case RVTrim::PseudoCCSLLW:
  case RVTrim::PseudoCCSRLW:
  case RVTrim::PseudoCCSRAW:
  case RVTrim::PseudoCCADDIW:
  case RVTrim::PseudoCCSLLIW:
  case RVTrim::PseudoCCSRLIW:
  case RVTrim::PseudoCCSRAIW:
  case RVTrim::PseudoCCANDN:
  case RVTrim::PseudoCCORN:
  case RVTrim::PseudoCCXNOR:
  case RVTrim::PseudoCCNDS_BFOS:
  case RVTrim::PseudoCCNDS_BFOZ:
    return expandCCOp(MBB, MBBI, NextMBBI);
  case RVTrim::PseudoVMCLR_M_B1:
  case RVTrim::PseudoVMCLR_M_B2:
  case RVTrim::PseudoVMCLR_M_B4:
  case RVTrim::PseudoVMCLR_M_B8:
  case RVTrim::PseudoVMCLR_M_B16:
  case RVTrim::PseudoVMCLR_M_B32:
  case RVTrim::PseudoVMCLR_M_B64:
    // vmclr.m vd => vmxor.mm vd, vd, vd
    return expandVMSET_VMCLR(MBB, MBBI, RVTrim::VMXOR_MM);
  case RVTrim::PseudoVMSET_M_B1:
  case RVTrim::PseudoVMSET_M_B2:
  case RVTrim::PseudoVMSET_M_B4:
  case RVTrim::PseudoVMSET_M_B8:
  case RVTrim::PseudoVMSET_M_B16:
  case RVTrim::PseudoVMSET_M_B32:
  case RVTrim::PseudoVMSET_M_B64:
    // vmset.m vd => vmxnor.mm vd, vd, vd
    return expandVMSET_VMCLR(MBB, MBBI, RVTrim::VMXNOR_MM);
  case RVTrim::PseudoReadVLENBViaVSETVLIX0:
    return expandPseudoReadVLENBViaVSETVLIX0(MBB, MBBI);
  case RVTrim::PseudoClearFPR64:
    return expandPseudoClearFPR64(MBB, MBBI);
  }

  return false;
}

bool RVTrimExpandPseudoPreEmitImpl::expandCCOp(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) const {
  // First try expanding to a Conditional Move rather than a branch+mv
  if (expandCCOpToCMov(MBB, MBBI))
    return true;

  MachineFunction *MF = MBB.getParent();
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  MachineBasicBlock *TrueBB = MF->CreateMachineBasicBlock(MBB.getBasicBlock());
  MachineBasicBlock *MergeBB = MF->CreateMachineBasicBlock(MBB.getBasicBlock());

  MF->insert(++MBB.getIterator(), TrueBB);
  MF->insert(++TrueBB->getIterator(), MergeBB);

  // We want to copy the "true" value only when the branch is executed.
  // The SDNodeXform is responsible for the inversion.
  unsigned BranchOpCode =
      MI.getOperand(MI.getNumExplicitOperands() - 3).getImm();

  // Insert branch instruction.
  BuildMI(MBB, MBBI, DL, TII->get(BranchOpCode))
      .add(MI.getOperand(MI.getNumExplicitOperands() - 2))
      .add(MI.getOperand(MI.getNumExplicitOperands() - 1))
      .addMBB(MergeBB);

  Register DestReg = MI.getOperand(0).getReg();
  assert(MI.getOperand(1).getReg() == DestReg);

  if (MI.getOpcode() == RVTrim::PseudoCCMOVGPR ||
      MI.getOpcode() == RVTrim::PseudoCCMOVGPRNoX0) {
    // Add MV.
    BuildMI(TrueBB, DL, TII->get(RVTrim::ADDI), DestReg)
        .add(MI.getOperand(2))
        .addImm(0);
  } else {
    unsigned NewOpc;
    // clang-format off
    switch (MI.getOpcode()) {
    default:
      llvm_unreachable("Unexpected opcode!");
    case RVTrim::PseudoCCADD:   NewOpc = RVTrim::ADD;   break;
    case RVTrim::PseudoCCSUB:   NewOpc = RVTrim::SUB;   break;
    case RVTrim::PseudoCCSLL:   NewOpc = RVTrim::SLL;   break;
    case RVTrim::PseudoCCSRL:   NewOpc = RVTrim::SRL;   break;
    case RVTrim::PseudoCCSRA:   NewOpc = RVTrim::SRA;   break;
    case RVTrim::PseudoCCAND:   NewOpc = RVTrim::AND;   break;
    case RVTrim::PseudoCCOR:    NewOpc = RVTrim::OR;    break;
    case RVTrim::PseudoCCXOR:   NewOpc = RVTrim::XOR;   break;
    case RVTrim::PseudoCCMAX:   NewOpc = RVTrim::MAX;   break;
    case RVTrim::PseudoCCMIN:   NewOpc = RVTrim::MIN;   break;
    case RVTrim::PseudoCCMAXU:  NewOpc = RVTrim::MAXU;  break;
    case RVTrim::PseudoCCMINU:  NewOpc = RVTrim::MINU;  break;
    case RVTrim::PseudoCCMUL:   NewOpc = RVTrim::MUL;   break;
    case RVTrim::PseudoCCLUI:   NewOpc = RVTrim::LUI;   break;
    case RVTrim::PseudoCCQC_E_LB:  NewOpc = RVTrim::QC_E_LB;    break;
    case RVTrim::PseudoCCQC_E_LH:  NewOpc = RVTrim::QC_E_LH;    break;
    case RVTrim::PseudoCCQC_E_LW:  NewOpc = RVTrim::QC_E_LW;    break;
    case RVTrim::PseudoCCQC_E_LHU: NewOpc = RVTrim::QC_E_LHU;   break;
    case RVTrim::PseudoCCQC_E_LBU: NewOpc = RVTrim::QC_E_LBU;   break;
    case RVTrim::PseudoCCLB:    NewOpc = RVTrim::LB;    break;
    case RVTrim::PseudoCCLH:    NewOpc = RVTrim::LH;    break;
    case RVTrim::PseudoCCLW:    NewOpc = RVTrim::LW;    break;
    case RVTrim::PseudoCCLHU:   NewOpc = RVTrim::LHU;   break;
    case RVTrim::PseudoCCLBU:   NewOpc = RVTrim::LBU;   break;
    case RVTrim::PseudoCCLWU:   NewOpc = RVTrim::LWU;   break;
    case RVTrim::PseudoCCLD:    NewOpc = RVTrim::LD;    break;
    case RVTrim::PseudoCCQC_LI:  NewOpc = RVTrim::QC_LI;   break;
    case RVTrim::PseudoCCQC_E_LI: NewOpc = RVTrim::QC_E_LI;   break;
    case RVTrim::PseudoCCADDI:  NewOpc = RVTrim::ADDI;  break;
    case RVTrim::PseudoCCSLLI:  NewOpc = RVTrim::SLLI;  break;
    case RVTrim::PseudoCCSRLI:  NewOpc = RVTrim::SRLI;  break;
    case RVTrim::PseudoCCSRAI:  NewOpc = RVTrim::SRAI;  break;
    case RVTrim::PseudoCCANDI:  NewOpc = RVTrim::ANDI;  break;
    case RVTrim::PseudoCCORI:   NewOpc = RVTrim::ORI;   break;
    case RVTrim::PseudoCCXORI:  NewOpc = RVTrim::XORI;  break;
    case RVTrim::PseudoCCADDW:  NewOpc = RVTrim::ADDW;  break;
    case RVTrim::PseudoCCSUBW:  NewOpc = RVTrim::SUBW;  break;
    case RVTrim::PseudoCCSLLW:  NewOpc = RVTrim::SLLW;  break;
    case RVTrim::PseudoCCSRLW:  NewOpc = RVTrim::SRLW;  break;
    case RVTrim::PseudoCCSRAW:  NewOpc = RVTrim::SRAW;  break;
    case RVTrim::PseudoCCADDIW: NewOpc = RVTrim::ADDIW; break;
    case RVTrim::PseudoCCSLLIW: NewOpc = RVTrim::SLLIW; break;
    case RVTrim::PseudoCCSRLIW: NewOpc = RVTrim::SRLIW; break;
    case RVTrim::PseudoCCSRAIW: NewOpc = RVTrim::SRAIW; break;
    case RVTrim::PseudoCCANDN:  NewOpc = RVTrim::ANDN;  break;
    case RVTrim::PseudoCCORN:   NewOpc = RVTrim::ORN;   break;
    case RVTrim::PseudoCCXNOR:  NewOpc = RVTrim::XNOR;  break;
    case RVTrim::PseudoCCNDS_BFOS: NewOpc = RVTrim::NDS_BFOS; break;
    case RVTrim::PseudoCCNDS_BFOZ: NewOpc = RVTrim::NDS_BFOZ; break;
    }
    // clang-format on

    if (NewOpc == RVTrim::NDS_BFOZ || NewOpc == RVTrim::NDS_BFOS) {
      BuildMI(TrueBB, DL, TII->get(NewOpc), DestReg)
          .add(MI.getOperand(2))
          .add(MI.getOperand(3))
          .add(MI.getOperand(4));
    } else if (NewOpc == RVTrim::LUI || NewOpc == RVTrim::QC_LI ||
               NewOpc == RVTrim::QC_E_LI) {
      BuildMI(TrueBB, DL, TII->get(NewOpc), DestReg).add(MI.getOperand(2));
    } else {
      BuildMI(TrueBB, DL, TII->get(NewOpc), DestReg)
          .add(MI.getOperand(2))
          .add(MI.getOperand(3));
    }
  }

  TrueBB->addSuccessor(MergeBB);

  MergeBB->splice(MergeBB->end(), &MBB, MI, MBB.end());
  MergeBB->transferSuccessors(&MBB);

  MBB.addSuccessor(TrueBB);
  MBB.addSuccessor(MergeBB);

  NextMBBI = MBB.end();
  MI.eraseFromParent();

  // Make sure live-ins are correctly attached to this new basic block.
  LivePhysRegs LiveRegs;
  computeAndAddLiveIns(LiveRegs, *TrueBB);
  computeAndAddLiveIns(LiveRegs, *MergeBB);

  return true;
}

bool RVTrimExpandPseudoPreEmitImpl::expandCCOpToCMov(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  if (MI.getOpcode() != RVTrim::PseudoCCMOVGPR &&
      MI.getOpcode() != RVTrim::PseudoCCMOVGPRNoX0)
    return false;

  if (!STI->hasVendorXqcicm())
    return false;

  MachineOperand &LHS = MI.getOperand(MI.getNumExplicitOperands() - 2);
  MachineOperand &RHS = MI.getOperand(MI.getNumExplicitOperands() - 1);

  // FIXME: Would be wonderful to support LHS=X0, but not very easy.
  if (LHS.getReg() == RVTrim::X0 || MI.getOperand(1).getReg() == RVTrim::X0 ||
      MI.getOperand(2).getReg() == RVTrim::X0)
    return false;

  // Use branch opcode to select appropriate Xqcicm instruction
  unsigned BCC = MI.getOperand(MI.getNumExplicitOperands() - 3).getImm();
  std::optional<unsigned> CMovRegOpcode;
  bool IsSigned = true;
  unsigned CMovImmOpcode;
  switch (BCC) {
  default:
    return false; // Unhandled branch opcodes
  case RVTrim::BNE:
    CMovRegOpcode = RVTrim::QC_MVEQ;
    CMovImmOpcode = RVTrim::QC_MVEQI;
    break;
  case RVTrim::BEQ:
    CMovRegOpcode = RVTrim::QC_MVNE;
    CMovImmOpcode = RVTrim::QC_MVNEI;
    break;
  case RVTrim::BGE:
    CMovRegOpcode = RVTrim::QC_MVLT;
    CMovImmOpcode = RVTrim::QC_MVLTI;
    break;
  case RVTrim::BLT:
    CMovRegOpcode = RVTrim::QC_MVGE;
    CMovImmOpcode = RVTrim::QC_MVGEI;
    break;
  case RVTrim::BGEU:
    CMovRegOpcode = RVTrim::QC_MVLTU;
    CMovImmOpcode = RVTrim::QC_MVLTUI;
    break;
  case RVTrim::BLTU:
    CMovRegOpcode = RVTrim::QC_MVGEU;
    CMovImmOpcode = RVTrim::QC_MVGEUI;
    break;
  case RVTrim::QC_BEQI:
    CMovImmOpcode = RVTrim::QC_MVNEI;
    break;
  case RVTrim::QC_BNEI:
    CMovImmOpcode = RVTrim::QC_MVEQI;
    break;
  case RVTrim::QC_BLTI:
    CMovImmOpcode = RVTrim::QC_MVGEI;
    break;
  case RVTrim::QC_BGEI:
    CMovImmOpcode = RVTrim::QC_MVLTI;
    break;
  case RVTrim::QC_BLTUI:
    CMovImmOpcode = RVTrim::QC_MVGEUI;
    IsSigned = false;
    break;
  case RVTrim::QC_BGEUI:
    CMovImmOpcode = RVTrim::QC_MVLTUI;
    IsSigned = false;
    break;
  }

  if (RHS.isImm()) {
    if ((!isInt<5>(RHS.getImm()) || !IsSigned) &&
        (!isUInt<5>(RHS.getImm()) || IsSigned))
      return false;

    // $dst = PseudoCCMOVGPR $falsev(=$dst), $truev, $opcode, $lhs, $rhs_imm
    // $dst = PseudoCCMOVGPRNoX0 $falsev(=$dst), $truev, $opcode, $lhs, $rhs_imm
    // =>
    // $dst = QC_MVccI $falsev (=$dst), $lhs, $rhs_imm, $truev
    BuildMI(MBB, MBBI, DL, TII->get(CMovImmOpcode))
        .addDef(MI.getOperand(0).getReg())
        .addReg(MI.getOperand(1).getReg())
        .addReg(LHS.getReg())
        .add(RHS)
        .addReg(MI.getOperand(2).getReg());

    MI.eraseFromParent();
    return true;
  }

  if (RHS.getReg() == RVTrim::X0) {
    // $dst = PseudoCCMOVGPR $falsev (=$dst), $truev, $opcode, $lhs, X0
    // $dst = PseudoCCMOVGPRNoX0 $falsev (=$dst), $truev, $opcode, $lhs, X0
    // =>
    // $dst = QC_MVccI $falsev (=$dst), $lhs, 0, $truev
    BuildMI(MBB, MBBI, DL, TII->get(CMovImmOpcode))
        .addDef(MI.getOperand(0).getReg())
        .addReg(MI.getOperand(1).getReg())
        .addReg(LHS.getReg())
        .addImm(0)
        .addReg(MI.getOperand(2).getReg());

    MI.eraseFromParent();
    return true;
  }

  if (!CMovRegOpcode)
    return false;

  // $dst = PseudoCCMOVGPR $falsev (=$dst), $truev, $opcode, $lhs, $rhs
  // $dst = PseudoCCMOVGPRNoX0 $falsev (=$dst), $truev, $opcode, $lhs, $rhs
  // =>
  // $dst = QC_MVcc $falsev (=$dst), $lhs, $rhs, $truev
  BuildMI(MBB, MBBI, DL, TII->get(*CMovRegOpcode))
      .addDef(MI.getOperand(0).getReg())
      .addReg(MI.getOperand(1).getReg())
      .addReg(LHS.getReg())
      .addReg(RHS.getReg())
      .addReg(MI.getOperand(2).getReg());
  MI.eraseFromParent();
  return true;
}

bool RVTrimExpandPseudoPreEmitImpl::expandVMSET_VMCLR(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    unsigned Opcode) const {
  DebugLoc DL = MBBI->getDebugLoc();
  Register DstReg = MBBI->getOperand(0).getReg();
  const MCInstrDesc &Desc = TII->get(Opcode);
  BuildMI(MBB, MBBI, DL, Desc, DstReg)
      .addReg(DstReg, RegState::Undef)
      .addReg(DstReg, RegState::Undef);
  MBBI->eraseFromParent(); // The pseudo instruction is gone now.
  return true;
}

bool RVTrimExpandPseudoPreEmitImpl::expandMV_FPR16INX(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  Register DstReg = TRI->getMatchingSuperReg(
      MBBI->getOperand(0).getReg(), RVTrim::sub_16, &RVTrim::GPRRegClass);
  Register SrcReg = TRI->getMatchingSuperReg(
      MBBI->getOperand(1).getReg(), RVTrim::sub_16, &RVTrim::GPRRegClass);

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::ADDI), DstReg)
      .addReg(SrcReg, getKillRegState(MBBI->getOperand(1).isKill()))
      .addImm(0);

  MBBI->eraseFromParent(); // The pseudo instruction is gone now.
  return true;
}

bool RVTrimExpandPseudoPreEmitImpl::expandMV_FPR32INX(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();
  const TargetRegisterInfo *TRI = STI->getRegisterInfo();
  Register DstReg = TRI->getMatchingSuperReg(
      MBBI->getOperand(0).getReg(), RVTrim::sub_32, &RVTrim::GPRRegClass);
  Register SrcReg = TRI->getMatchingSuperReg(
      MBBI->getOperand(1).getReg(), RVTrim::sub_32, &RVTrim::GPRRegClass);

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::ADDI), DstReg)
      .addReg(SrcReg, getKillRegState(MBBI->getOperand(1).isKill()))
      .addImm(0);

  MBBI->eraseFromParent(); // The pseudo instruction is gone now.
  return true;
}

// This function expands the PseudoRV32ZdinxSD for storing a double-precision
// floating-point value into memory by generating an equivalent instruction
// sequence for RV32.
bool RVTrimExpandPseudoPreEmitImpl::expandRV32ZdinxStore(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();
  const RVTrimRegisterInfo *TRI = STI->getRegisterInfo();
  auto [Lo, Hi] =
      TRI->getGPRPairSubRegs(MBBI->getOperand(0).getReg().asMCReg());

  auto MIBLo = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::SW))
                   .addReg(Lo, getKillRegState(MBBI->getOperand(0).isKill()))
                   .addReg(MBBI->getOperand(1).getReg())
                   .add(MBBI->getOperand(2));

  MachineInstrBuilder MIBHi;
  if (MBBI->getOperand(2).isGlobal() || MBBI->getOperand(2).isCPI()) {
    assert(MBBI->getOperand(2).getOffset() % 8 == 0);
    MBBI->getOperand(2).setOffset(MBBI->getOperand(2).getOffset() + 4);
    MIBHi = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::SW))
                .addReg(Hi, getKillRegState(MBBI->getOperand(0).isKill()))
                .add(MBBI->getOperand(1))
                .add(MBBI->getOperand(2));
  } else {
    assert(isInt<12>(MBBI->getOperand(2).getImm() + 4));
    MIBHi = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::SW))
                .addReg(Hi, getKillRegState(MBBI->getOperand(0).isKill()))
                .add(MBBI->getOperand(1))
                .addImm(MBBI->getOperand(2).getImm() + 4);
  }

  MachineFunction *MF = MBB.getParent();
  SmallVector<MachineMemOperand *> NewLoMMOs;
  SmallVector<MachineMemOperand *> NewHiMMOs;
  for (const MachineMemOperand *MMO : MBBI->memoperands()) {
    NewLoMMOs.push_back(MF->getMachineMemOperand(MMO, 0, 4));
    NewHiMMOs.push_back(MF->getMachineMemOperand(MMO, 4, 4));
  }
  MIBLo.setMemRefs(NewLoMMOs);
  MIBHi.setMemRefs(NewHiMMOs);

  MBBI->eraseFromParent();
  return true;
}

// This function expands PseudoRV32ZdinxLoad for loading a double-precision
// floating-point value from memory into an equivalent instruction sequence for
// RV32.
bool RVTrimExpandPseudoPreEmitImpl::expandRV32ZdinxLoad(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();
  const RVTrimRegisterInfo *TRI = STI->getRegisterInfo();
  auto [Lo, Hi] =
      TRI->getGPRPairSubRegs(MBBI->getOperand(0).getReg().asMCReg());

  MachineInstrBuilder MIBLo, MIBHi;

  // If the register of operand 1 is equal to the Lo register, then swap the
  // order of loading the Lo and Hi statements.
  bool IsOp1EqualToLo = Lo == MBBI->getOperand(1).getReg();
  // Order: Lo, Hi
  if (!IsOp1EqualToLo) {
    MIBLo = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::LW), Lo)
                .addReg(MBBI->getOperand(1).getReg())
                .add(MBBI->getOperand(2));
  }

  if (MBBI->getOperand(2).isGlobal() || MBBI->getOperand(2).isCPI()) {
    auto Offset = MBBI->getOperand(2).getOffset();
    assert(Offset % 8 == 0);
    MBBI->getOperand(2).setOffset(Offset + 4);
    MIBHi = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::LW), Hi)
                .addReg(MBBI->getOperand(1).getReg())
                .add(MBBI->getOperand(2));
    MBBI->getOperand(2).setOffset(Offset);
  } else {
    assert(isInt<12>(MBBI->getOperand(2).getImm() + 4));
    MIBHi = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::LW), Hi)
                .addReg(MBBI->getOperand(1).getReg())
                .addImm(MBBI->getOperand(2).getImm() + 4);
  }

  // Order: Hi, Lo
  if (IsOp1EqualToLo) {
    MIBLo = BuildMI(MBB, MBBI, DL, TII->get(RVTrim::LW), Lo)
                .addReg(MBBI->getOperand(1).getReg())
                .add(MBBI->getOperand(2));
  }

  MachineFunction *MF = MBB.getParent();
  SmallVector<MachineMemOperand *> NewLoMMOs;
  SmallVector<MachineMemOperand *> NewHiMMOs;
  for (const MachineMemOperand *MMO : MBBI->memoperands()) {
    NewLoMMOs.push_back(MF->getMachineMemOperand(MMO, 0, 4));
    NewHiMMOs.push_back(MF->getMachineMemOperand(MMO, 4, 4));
  }
  MIBLo.setMemRefs(NewLoMMOs);
  MIBHi.setMemRefs(NewHiMMOs);

  MBBI->eraseFromParent();
  return true;
}

bool RVTrimExpandPseudoPreEmitImpl::expandPseudoReadVLENBViaVSETVLIX0(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  DebugLoc DL = MBBI->getDebugLoc();
  Register Dst = MBBI->getOperand(0).getReg();
  unsigned Mul = MBBI->getOperand(1).getImm();
  RISCVVType::VLMUL VLMUL = RISCVVType::encodeLMUL(Mul, /*Fractional=*/false);
  unsigned VTypeImm = RISCVVType::encodeVTYPE(
      VLMUL, /*SEW=*/8, /*TailAgnostic=*/true, /*MaskAgnostic=*/true);

  BuildMI(MBB, MBBI, DL, TII->get(RVTrim::PseudoVSETVLIX0), Dst)
      .addReg(RVTrim::X0, RegState::Kill)
      .addImm(VTypeImm);

  MBBI->eraseFromParent();
  return true;
}

bool RVTrimExpandPseudoPreEmitImpl::expandPseudoClearFPR64(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI) const {
  const DebugLoc &DL = MBBI->getDebugLoc();
  Register Dst = MBBI->getOperand(0).getReg();

  if (STI->is64Bit()) {
    BuildMI(MBB, MBBI, DL, TII->get(RVTrim::FMV_D_X), Dst).addReg(RVTrim::X0);
  } else {
    BuildMI(MBB, MBBI, DL, TII->get(RVTrim::FCVT_D_W), Dst)
        .addReg(RVTrim::X0)
        .addImm(RVTrimFPRndMode::RNE);
  }

  MBBI->eraseFromParent();
  return true;
}

char RVTrimExpandPseudoPreEmitLegacy::ID = 0;

INITIALIZE_PASS(RVTrimExpandPseudoPreEmitLegacy, "rvtrim-expand-pseudo-pre-emit",
                RVTrim_EXPAND_PSEUDO_PRE_EMIT_NAME, false, false)

FunctionPass *llvm::createRVTrimExpandPseudoPreEmitLegacyPass() {
  return new RVTrimExpandPseudoPreEmitLegacy();
}

PreservedAnalyses
RVTrimExpandPseudoPreEmitPass::run(MachineFunction &MF,
                                  MachineFunctionAnalysisManager &MFAM) {
  bool Changed = RVTrimExpandPseudoPreEmitImpl().run(MF);
  if (!Changed)
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses();
}
