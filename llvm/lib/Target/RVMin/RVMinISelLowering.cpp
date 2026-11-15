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
#include "RVMinBaseInfo.h"
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
  case RVMinISD::CALL:
    return "RVMinISD::CALL";
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

  MachineFrameInfo &MFI = MF.getFrameInfo();

  for (unsigned I = 0, E = Ins.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    MVT LocVT = VA.getLocVT();

    if (VA.isRegLoc()) {
      // Arguments 1..8 arrive in a0-a7.
      Register VReg = RegInfo.createVirtualRegister(&RVMin::GPRRegClass);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, LocVT));
      continue;
    }

    // Stack arguments live in the caller's frame, at entry_sp + offset.  Model
    // each one as an immutable fixed object and load it like any other stack
    // slot: the frame-index machinery from chapter 6 turns the load into a
    // single `lw rd, off(sp)`.
    assert(VA.isMemLoc() && "argument must be in a register or on the stack");
    unsigned Size = LocVT.getSizeInBits() / 8;
    int FI = MFI.CreateFixedObject(Size, VA.getLocMemOffset(),
                                   /*IsImmutable=*/true);
    SDValue FIN = DAG.getFrameIndex(FI, getPointerTy(DAG.getDataLayout()));
    InVals.push_back(DAG.getLoad(LocVT, DL, Chain, FIN,
                                 MachinePointerInfo::getFixedStack(MF, FI)));
  }

  return Chain;
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//
//
// The shape of a call in the DAG:
//
//   CALLSEQ_START <outgoing-args-size>      (ADJCALLSTACKDOWN)
//     store stack arguments to sp + offset
//     a0..a7 = argument values              (CopyToReg)
//     RVMinISD::CALL chain, callee, a0..a7, regmask
//   CALLSEQ_END                             (ADJCALLSTACKUP)
//     results = a0, a1                      (CopyFromReg)
//
// RVMin has no frame pointer and never adjusts sp around a call: the outgoing
// argument area is reserved once in the prologue (hasReservedCallFrame() is
// true), so CALLSEQ_* lower to pseudos that PEI simply deletes.

SDValue RVMinTargetLowering::LowerCall(CallLoweringInfo &CLI,
                                       SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc &DL = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins = CLI.Ins;
  SDValue Chain = CLI.Chain;
  SDValue Callee = CLI.Callee;
  CallingConv::ID CallConv = CLI.CallConv;
  bool IsVarArg = CLI.IsVarArg;

  // Tail calls need a callee that can reuse the caller's frame; leave them for
  // a later chapter and let the caller emit a normal call + return.
  CLI.IsTailCall = false;

  if (IsVarArg)
    report_fatal_error("RVMin: varargs are not supported yet");

  MachineFunction &MF = DAG.getMachineFunction();
  EVT PtrVT = getPointerTy(DAG.getDataLayout());

  // Assign a location to every outgoing argument: a0-a7 first, then the stack.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_RVMin);

  // Size of the outgoing argument area.  The DAG builder records it in
  // CALLSEQ_START and PEI adds it to the fixed frame.
  unsigned NumBytes = CCInfo.getStackSize();
  Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, DL);

  SmallVector<std::pair<Register, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;
  SDValue StackPtr;

  for (unsigned I = 0, E = ArgLocs.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    SDValue ArgValue = OutVals[I];

    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), ArgValue));
      continue;
    }

    assert(VA.isMemLoc() && "argument must be in a register or on the stack");

    // Stack argument number N lives at sp + LocMemOffset.  The address is
    // `sp + constant`; the store pattern folds the constant into the S-type
    // immediate of a single `sw`.
    if (!StackPtr.getNode())
      StackPtr = DAG.getCopyFromReg(Chain, DL, RVMin::X2, PtrVT);
    SDValue Address = DAG.getNode(
        ISD::ADD, DL, PtrVT, StackPtr,
        DAG.getIntPtrConstant(VA.getLocMemOffset(), DL));
    MemOpChains.push_back(DAG.getStore(
        Chain, DL, ArgValue, Address,
        MachinePointerInfo::getStack(MF, VA.getLocMemOffset())));
  }

  // The stores are independent of each other and of the argument copies.
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);

  // Copy the register arguments.  The copies are chained and glued so they
  // cannot be scheduled after the call.
  SDValue Glue;
  for (auto &Reg : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, DL, Reg.first, Reg.second, Glue);
    Glue = Chain.getValue(1);
  }

  // A direct callee becomes a target node so it survives legalization and can
  // be matched by the PseudoCALL patterns.  An indirect callee stays a value.
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), DL, PtrVT,
                                        G->getOffset(), RVMinII::MO_CALL);
  else if (auto *S = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), PtrVT, RVMinII::MO_CALL);

  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  // Argument registers must be live into the call.
  for (auto &Reg : RegsToPass)
    Ops.push_back(DAG.getRegister(Reg.first, Reg.second.getValueType()));

  // The call-preserved mask: what the register allocator may keep in
  // registers across this call (ra and s0-s11).
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  const uint32_t *Mask = TRI->getCallPreservedMask(MF, CallConv);
  assert(Mask && "RVMin: missing call-preserved register mask");
  Ops.push_back(DAG.getRegisterMask(Mask));

  if (Glue.getNode())
    Ops.push_back(Glue);

  Chain = DAG.getNode(RVMinISD::CALL, DL, DAG.getVTList(MVT::Other, MVT::Glue),
                      Ops);
  Glue = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain, NumBytes, 0, Glue, DL);
  Glue = Chain.getValue(1);

  // Copy the results out of a0/a1.
  SmallVector<CCValAssign, 4> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(Ins, RetCC_RVMin);

  for (CCValAssign &VA : RVLocs) {
    assert(VA.isRegLoc() && "only register results are supported");
    SDValue Copy =
        DAG.getCopyFromReg(Chain, DL, VA.getLocReg(), VA.getLocVT(), Glue);
    InVals.push_back(Copy.getValue(0));
    Chain = Copy.getValue(1);
    Glue = Copy.getValue(2);
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

  // Note that `ra` is deliberately not an operand of the RET node: the return
  // instruction is `jalr zero, 0(ra)` only at the MC level.  It does not need
  // to be live-in either.  `ra` is callee-saved, so if anything in this
  // function writes it (a call, or the register allocator picking it as the
  // last scratch register), PEI saves the incoming value in the prologue and
  // reloads it before the return.
  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(RVMinISD::RET_FLAG, DL, MVT::Other, RetOps);
}
