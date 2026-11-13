//===-- MiniRVISelLowering.cpp - MiniRV DAG Lowering Interface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lowering is intentionally narrow: the corpus covers 64-bit integer
// arithmetic, and there is no memory, branch or call instruction, so anything
// that would need one is reported instead of being miscompiled.
//
//===----------------------------------------------------------------------===//

#include "MiniRVISelLowering.h"
#include "MiniRV.h"
#include "MiniRVSelectionDAGInfo.h"
#include "MiniRVSubtarget.h"
#include "MiniRVTargetMachine.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "MiniRVGenCallingConv.inc"

MiniRVTargetLowering::MiniRVTargetLowering(const TargetMachine &TM,
                                           const MiniRVSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i64, &MiniRV::GPRRegClass);

  // There is no multiply or divide instruction.
  for (MVT VT : {MVT::i64}) {
    setOperationAction(ISD::MUL, VT, Expand);
    setOperationAction(ISD::SDIV, VT, Expand);
    setOperationAction(ISD::UDIV, VT, Expand);
    setOperationAction(ISD::SREM, VT, Expand);
    setOperationAction(ISD::UREM, VT, Expand);
  }

  setOperationAction(ISD::BRCOND, MVT::Other, Custom);
  // BR_CC is expanded into BRCOND plus a comparison, which is then fused back
  // into one branch by LowerBRCOND.
  setOperationAction(ISD::BR_CC, MVT::i64, Expand);

  setStackPointerRegisterToSaveRestore(MiniRV::X2);
  setBooleanContents(ZeroOrOneBooleanContent);
  setMaxAtomicSizeInBitsSupported(0);

  computeRegisterProperties(STI.getRegisterInfo());
}

EVT MiniRVTargetLowering::getSetCCResultType(const DataLayout &DL,
                                             LLVMContext &Context,
                                             EVT VT) const {
  // There is no i1 register, so a comparison produces a full xlen value that
  // is either zero or one (setBooleanContents() above).
  return getPointerTy(DL);
}

SDValue MiniRVTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::BRCOND:
    return LowerBRCOND(Op, DAG);
  default:
    llvm_unreachable("MiniRV does not custom-lower this operation");
  }
}

/// Build MiniRVISD::BRCC, which the patterns in MiniRVInstrInfo.td match
/// directly against the branch records.
SDValue MiniRVTargetLowering::LowerBranchCC(SDValue Chain, SDValue LHS,
                                            SDValue RHS, ISD::CondCode CC,
                                            SDValue Dest, const SDLoc &DL,
                                            SelectionDAG &DAG) const {
  // The corpus has the "less than" and "greater or equal" records only, so the
  // two other ordered conditions swap their operands instead of asking for a
  // record that does not exist.
  switch (CC) {
  case ISD::SETGT:
    std::swap(LHS, RHS);
    CC = ISD::SETLT;
    break;
  case ISD::SETLE:
    std::swap(LHS, RHS);
    CC = ISD::SETGE;
    break;
  case ISD::SETUGT:
    std::swap(LHS, RHS);
    CC = ISD::SETULT;
    break;
  case ISD::SETULE:
    std::swap(LHS, RHS);
    CC = ISD::SETUGE;
    break;
  default:
    break;
  }

  return DAG.getNode(MiniRVISD::BRCC, DL, MVT::Other, Chain, LHS, RHS,
                     DAG.getCondCode(CC), Dest);
}

/// A condition that is not a comparison is compared against zero.
SDValue MiniRVTargetLowering::LowerBRCOND(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Cond = Op.getOperand(1);
  SDValue Dest = Op.getOperand(2);
  SDLoc DL(Op);

  if (Cond.getOpcode() == ISD::SETCC) {
    ISD::CondCode CC = cast<CondCodeSDNode>(Cond.getOperand(2))->get();
    return LowerBranchCC(Chain, Cond.getOperand(0), Cond.getOperand(1), CC,
                         Dest, DL, DAG);
  }

  SDValue Zero = DAG.getConstant(0, DL, MVT::i64);
  return LowerBranchCC(Chain, Cond, Zero, ISD::SETNE, Dest, DL, DAG);
}

SDValue MiniRVTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_MiniRV);

  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  for (CCValAssign &VA : ArgLocs) {
    if (!VA.isRegLoc())
      report_fatal_error("MiniRV cannot read a stack argument yet");

    // Go through a virtual register so that the physical argument register is
    // recorded as live-in to the function.
    Register VReg = RegInfo.createVirtualRegister(&MiniRV::GPRRegClass);
    RegInfo.addLiveIn(VA.getLocReg(), VReg);
    SDValue Arg = DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT());
    Chain = Arg.getValue(1);
    InVals.push_back(Arg);
  }

  return Chain;
}

bool MiniRVTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_MiniRV);
}

SDValue
MiniRVTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                                  bool IsVarArg,
                                  const SmallVectorImpl<ISD::OutputArg> &Outs,
                                  const SmallVectorImpl<SDValue> &OutVals,
                                  const SDLoc &DL, SelectionDAG &DAG) const {
  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_MiniRV);

  SDValue Glue;
  SmallVector<SDValue, 4> RetOps(1, Chain);
  for (unsigned I = 0; I != RVLocs.size(); ++I) {
    CCValAssign &VA = RVLocs[I];
    assert(VA.isRegLoc() && "MiniRV returns in registers only");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[I], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(MiniRVISD::RET, DL, MVT::Other, RetOps);
}

SDValue
MiniRVTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
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

  if (CLI.IsTailCall)
    report_fatal_error("MiniRV does not lower tail calls");
  if (IsVarArg)
    report_fatal_error("MiniRV does not support variadic calls");

  MachineFunction &MF = DAG.getMachineFunction();

  // Analyse the arguments: eight registers, nothing on the stack.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_MiniRV);

  if (CCInfo.getStackSize() != 0)
    report_fatal_error("MiniRV cannot pass arguments on the stack");

  // The callee has to be a symbol: the corpus has no way to compute the
  // address of a function, so an indirect call cannot be lowered.  A direct
  // callee becomes a target node so that the patterns can match it.
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee =
        DAG.getTargetGlobalAddress(G->getGlobal(), DL, PtrVT, G->getOffset());
  else if (ExternalSymbolSDNode *S = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), PtrVT);
  else
    report_fatal_error("MiniRV can only call a direct symbol");

  // Move the arguments into their registers, chaining the copies so that the
  // call reads them in order.
  SDValue Glue;
  SmallVector<SDValue, 8> ArgRegs;
  for (unsigned I = 0, E = ArgLocs.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    assert(VA.isRegLoc() && "stack argument in a call");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[I], Glue);
    Glue = Chain.getValue(1);
    ArgRegs.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);

  // Naming the argument registers here makes them implicit uses of the call,
  // which keeps the copies that fill them alive.
  append_range(Ops, ArgRegs);

  // The register mask tells the allocator which registers survive the call.
  const uint32_t *Mask =
      MF.getSubtarget().getRegisterInfo()->getCallPreservedMask(MF, CallConv);
  assert(Mask && "MiniRV is missing a call-preserved register mask");
  Ops.push_back(DAG.getRegisterMask(Mask));

  // The glue goes last on a variadic call node.
  if (Glue.getNode())
    Ops.push_back(Glue);
  Chain = DAG.getNode(MiniRVISD::CALL, DL, NodeTys, Ops);
  Glue = Chain.getValue(1);

  // Copy the result out of x10.
  SmallVector<CCValAssign, 4> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(Ins, RetCC_MiniRV);
  for (CCValAssign &VA : RVLocs) {
    SDValue Ret =
        DAG.getCopyFromReg(Chain, DL, VA.getLocReg(), VA.getLocVT(), Glue);
    Chain = Ret.getValue(1);
    Glue = Ret.getValue(2);
    InVals.push_back(Ret.getValue(0));
  }

  return Chain;
}

const char *MiniRVTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case MiniRVISD::BRCC:
    return "MiniRVISD::BRCC";
  case MiniRVISD::CALL:
    return "MiniRVISD::CALL";
  case MiniRVISD::RET:
    return "MiniRVISD::RET";
  default:
    return nullptr;
  }
}
