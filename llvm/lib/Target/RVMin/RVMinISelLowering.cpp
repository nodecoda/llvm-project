//===-- RVMinISelLowering.cpp - RVMin DAG lowering implementation --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 3: just enough lowering to compile
//
//   int f() { return 42; }
//   int f(int a, int b) { return a + b; }
//
// That means formal arguments in a0-a7, returns in a0, and the RET_FLAG node.
// Calls, stack arguments and everything else arrive in later chapters.
//
//===----------------------------------------------------------------------===//

#include "RVMinISelLowering.h"
#include "RVMin.h"
#include "RVMinSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "RVMinGenCallingConv.inc"

RVMinTargetLowering::RVMinTargetLowering(const TargetMachine &TM,
                                         const RVMinSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &RVMin::GPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  setStackPointerRegisterToSaveRestore(RVMin::X2);
  setBooleanContents(ZeroOrOneBooleanContent);
  setBooleanVectorContents(ZeroOrOneBooleanContent);

  // Chapter 4: i32 ALU operations are all legal (one RV32I instruction each).
  // Comparisons are legal too: TableGen patterns turn every condcode into
  // SLT/SLTU/SLTI/SLTIU plus, where needed, one XORI to invert the result.
  for (MVT VT : {MVT::i32}) {
    setOperationAction(ISD::SELECT, VT, Expand);
    setOperationAction(ISD::SELECT_CC, VT, Custom);
  }

  // Chapter 5: conditional branches.  BRCOND is lowered to a target BR_CC node,
  // which TableGen matches against BEQ/BNE/BLT/BGE/BLTU/BGEU.
  setOperationAction(ISD::BRCOND, MVT::Other, Custom);
  setOperationAction(ISD::BR_CC, MVT::i32, Custom);
}

// Comparisons produce a 0/1 integer, not an i1: RV32I has no i1 registers and
// the DAG is simpler if setcc results are already in a GPR.
EVT RVMinTargetLowering::getSetCCResultType(const DataLayout &DL,
                                            LLVMContext &Context,
                                            EVT VT) const {
  return getPointerTy(DL);
}

SDValue RVMinTargetLowering::LowerOperation(SDValue Op,
                                            SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  default:
    llvm_unreachable("RVMin: unhandled custom lowering");
  case ISD::SELECT_CC:
    return lowerSELECT_CC(Op, DAG);
  case ISD::BRCOND:
    return lowerBRCOND(Op, DAG);
  case ISD::BR_CC:
    return lowerBR_CC(Op, DAG);
  }
}

// Canonicalize the condition code so the TableGen patterns only need
// SETEQ/SETNE/SETLT/SETGE/SETULT/SETUGE.  The two "greater" forms and the
// non-strict unsigned forms are the same comparison with swapped operands or
// an inverted result:
//
//   a > b   ==  b < a          a <= b  ==  b >= a
//   a >u b  ==  b <u a         a <=u b == b >=u a
static void canonicalizeBranchCC(ISD::CondCode &CC, SDValue &LHS,
                                 SDValue &RHS) {
  switch (CC) {
  default:
    return;
  case ISD::SETGT:
    CC = ISD::SETLT;
    std::swap(LHS, RHS);
    return;
  case ISD::SETLE:
    CC = ISD::SETGE;
    std::swap(LHS, RHS);
    return;
  case ISD::SETUGT:
    CC = ISD::SETULT;
    std::swap(LHS, RHS);
    return;
  case ISD::SETULE:
    CC = ISD::SETUGE;
    std::swap(LHS, RHS);
    return;
  }
}

// BRCOND always carries a boolean in the DAG.  When it came from a setcc we
// can branch on the original comparison operands directly; otherwise compare
// the boolean against zero.
SDValue RVMinTargetLowering::lowerBRCOND(SDValue Op,
                                         SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Cond = Op.getOperand(1);
  SDValue Dest = Op.getOperand(2);
  SDLoc DL(Op);

  if (Cond.getOpcode() == ISD::SETCC) {
    SDValue LHS = Cond.getOperand(0);
    SDValue RHS = Cond.getOperand(1);
    ISD::CondCode CCVal = cast<CondCodeSDNode>(Cond.getOperand(2))->get();
    canonicalizeBranchCC(CCVal, LHS, RHS);
    return DAG.getNode(RVMinISD::BR_CC, DL, MVT::Other, Chain, LHS, RHS,
                       DAG.getCondCode(CCVal), Dest);
  }

  return DAG.getNode(RVMinISD::BR_CC, DL, MVT::Other, Chain, Cond,
                     DAG.getConstant(0, DL, MVT::i32),
                     DAG.getCondCode(ISD::SETNE), Dest);
}

SDValue RVMinTargetLowering::lowerBR_CC(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue LHS = Op.getOperand(2);
  SDValue RHS = Op.getOperand(3);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(1))->get();
  SDValue Dest = Op.getOperand(4);
  SDLoc DL(Op);

  canonicalizeBranchCC(CC, LHS, RHS);
  return DAG.getNode(RVMinISD::BR_CC, DL, MVT::Other, Chain, LHS, RHS,
                     DAG.getCondCode(CC), Dest);
}

// RV32I has no conditional move.  Chapter 4 lowers select branchlessly with a
// mask:
//
//   cond = setcc lhs, rhs        // 0 or 1
//   mask = 0 - cond              // 0x00000000 or 0xffffffff
//   res  = false ^ ((true ^ false) & mask)
//
// Chapter 5 revisits this with a real branch, which is usually shorter.
SDValue RVMinTargetLowering::lowerSELECT_CC(SDValue Op,
                                            SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  SDValue TrueV = Op.getOperand(2);
  SDValue FalseV = Op.getOperand(3);
  ISD::CondCode CC = Op.getOperandCondCode(4);
  EVT VT = Op.getValueType();

  EVT CCVT = getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);
  SDValue Cond = DAG.getNode(ISD::SETCC, DL, CCVT, LHS, RHS,
                             DAG.getCondCode(CC));
  SDValue Mask = DAG.getNode(ISD::SUB, DL, VT, DAG.getConstant(0, DL, VT), Cond);
  SDValue Diff = DAG.getNode(ISD::XOR, DL, VT, TrueV, FalseV);
  SDValue Sel = DAG.getNode(ISD::AND, DL, VT, Diff, Mask);
  return DAG.getNode(ISD::XOR, DL, VT, Sel, FalseV);
}

const char *RVMinTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case RVMinISD::RET_FLAG:
    return "RVMinISD::RET_FLAG";
  case RVMinISD::BR_CC:
    return "RVMinISD::BR_CC";
  default:
    return nullptr;
  }
}

SDValue RVMinTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  if (IsVarArg)
    report_fatal_error("RVMin: varargs are not supported yet");

  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_RVMin);

  for (unsigned I = 0, E = Ins.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    if (!VA.isRegLoc())
      report_fatal_error("RVMin: stack arguments are not supported yet");

    MVT LocVT = VA.getLocVT();
    Register VReg = RegInfo.createVirtualRegister(&RVMin::GPRRegClass);
    RegInfo.addLiveIn(VA.getLocReg(), VReg);
    SDValue ArgValue = DAG.getCopyFromReg(Chain, DL, VReg, LocVT);
    InVals.push_back(ArgValue);
  }

  return Chain;
}

bool RVMinTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_RVMin);
}

SDValue RVMinTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  if (!CCInfo.CheckReturn(Outs, RetCC_RVMin))
    return DAG.getNode(ISD::UNDEF, DL, MVT::Other);

  // Move the return values into their ABI registers and terminate with
  // RET_FLAG, which TableGen maps to PseudoRET / `jalr zero, 0(ra)`.
  SmallVector<SDValue, 4> RetOps(1, Chain);
  SDValue Glue;
  for (unsigned I = 0, E = RVLocs.size(); I != E; ++I) {
    CCValAssign &VA = RVLocs[I];
    assert(VA.isRegLoc() && "can only return in registers");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[I], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(RVMinISD::RET_FLAG, DL, MVT::Other, RetOps);
}
