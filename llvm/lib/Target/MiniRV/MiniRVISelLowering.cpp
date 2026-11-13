//===-- MiniRVISelLowering.cpp - MiniRV DAG Lowering Interface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lowering covers the RV64IM integer core: 64-bit and 32-bit arithmetic,
// byte/halfword/word memory access, branches, selects, calls (including stack
// arguments) and returns.  i8, i16 and i32 are promoted to i64, which keeps
// the DAG free of sub-word types while the narrow memory instructions and the
// *W arithmetic instructions still model the smaller widths.
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

#include <tuple>

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "MiniRVGenCallingConv.inc"

MiniRVTargetLowering::MiniRVTargetLowering(const TargetMachine &TM,
                                           const MiniRVSubtarget &STI)
    : TargetLowering(TM, STI) {
  // Only XLEN is a legal integer type; i8/i16/i32 are promoted to i64.  The
  // 32-bit operations that have a *W instruction are custom-lowered so that
  // the promotion does not turn them into 64-bit operations.
  addRegisterClass(MVT::i64, &MiniRV::GPRRegClass);

  // RV64M supplies the multiply, divide and remainder instructions.
  setOperationAction(ISD::MUL, MVT::i64, Legal);
  setOperationAction(ISD::SDIV, MVT::i64, Legal);
  setOperationAction(ISD::UDIV, MVT::i64, Legal);
  setOperationAction(ISD::SREM, MVT::i64, Legal);
  setOperationAction(ISD::UREM, MVT::i64, Legal);

  // The *W instructions only exist for arithmetic and shifts.  The bitwise
  // operations stay 64-bit instructions, so their i32 form is custom-lowered
  // to a 64-bit operation followed by a sign extension of the low word.
  for (unsigned Opc :
       {ISD::ADD, ISD::SUB, ISD::SHL, ISD::SRL, ISD::SRA, ISD::MUL, ISD::SDIV,
        ISD::UDIV, ISD::SREM, ISD::UREM, ISD::AND, ISD::OR, ISD::XOR})
    setOperationAction(Opc, MVT::i32, Custom);

  // Narrow loads and stores are promoted to i64; the extend/truncate flavour
  // of the node selects LB/LBU/LH/LHU/LW/LWU and SB/SH/SW.
  setLoadExtAction({ISD::SEXTLOAD, ISD::ZEXTLOAD, ISD::EXTLOAD}, MVT::i64,
                   MVT::i8, Legal);
  setLoadExtAction({ISD::SEXTLOAD, ISD::ZEXTLOAD, ISD::EXTLOAD}, MVT::i64,
                   MVT::i16, Legal);
  setLoadExtAction({ISD::SEXTLOAD, ISD::ZEXTLOAD, ISD::EXTLOAD}, MVT::i64,
                   MVT::i32, Legal);
  setTruncStoreAction(MVT::i64, MVT::i8, Legal);
  setTruncStoreAction(MVT::i64, MVT::i16, Legal);
  setTruncStoreAction(MVT::i64, MVT::i32, Legal);

  // There is no conditional move in the corpus: a select is first rewritten
  // to SELECT_CC, which is then lowered to mask arithmetic.
  setOperationAction(ISD::SELECT, MVT::i64, Expand);
  setOperationAction(ISD::SELECT, MVT::i32, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::i64, Custom);
  setOperationAction(ISD::SELECT_CC, MVT::i32, Custom);

  // The address of a symbol is materialised with auipc+addi.
  setOperationAction(ISD::GlobalAddress, MVT::i64, Custom);

  setOperationAction(ISD::BRCOND, MVT::Other, Custom);
  // BR_CC is expanded into BRCOND plus a comparison, which is then fused back
  // into one branch by LowerBRCOND.
  setOperationAction(ISD::BR_CC, MVT::i64, Expand);
  setOperationAction(ISD::BR_CC, MVT::i32, Expand);

  // A variable-length alloca moves the stack pointer at run time.  The
  // generic expansion is exactly the sequence MiniRV needs: subtract the size
  // (already rounded up to the stack alignment by the DAG builder) from the
  // stack pointer and copy the result back into it.  A non-zero alignment
  // operand is masked in by the same expansion.
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i64, Expand);

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
  case ISD::GlobalAddress:
    return LowerGlobalAddress(Op, DAG);
  case ISD::BRCOND:
    return LowerBRCOND(Op, DAG);
  case ISD::SELECT_CC:
    return LowerSELECT_CC(Op, DAG);
  default:
    llvm_unreachable("MiniRV does not custom-lower this operation");
  }
}

/// The address of a symbol: auipc rd, %pcrel_hi(sym) adds the high 20 bits of
/// the PC-relative distance to the PC, then addi rd, rd, %pcrel_lo(auipc)
/// adds the low 12.  The pair stays a pseudo until the AsmPrinter, which has
/// to create the label that ties the two parts together.
SDValue MiniRVTargetLowering::LowerGlobalAddress(SDValue Op,
                                                 SelectionDAG &DAG) const {
  const auto *GV = cast<GlobalAddressSDNode>(Op);
  SDLoc DL(Op);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue Sym =
      DAG.getTargetGlobalAddress(GV->getGlobal(), DL, PtrVT, GV->getOffset());
  return SDValue(DAG.getMachineNode(MiniRV::PseudoLA, DL, PtrVT, Sym), 0);
}

/// Turn an i32 operation into its *W form.  The operands are any-extended:
/// the W instructions only read the low 32 bits, and they write a
/// sign-extended 32-bit result, which is the representation an i32 value has
/// after promotion.
void MiniRVTargetLowering::ReplaceNodeResults(SDNode *N,
                                              SmallVectorImpl<SDValue> &Results,
                                              SelectionDAG &DAG) const {
  SDLoc DL(N);

  auto LowerW = [&](unsigned Opcode) {
    SDValue LHS = DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i64, N->getOperand(0));
    SDValue RHS = DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i64, N->getOperand(1));
    SDValue Res = DAG.getNode(Opcode, DL, MVT::i64, LHS, RHS);
    Results.push_back(DAG.getNode(ISD::TRUNCATE, DL, N->getValueType(0), Res));
  };

  // The bitwise operations have no *W instruction.  They are done in 64 bits
  // and the result is sign-extended from bit 31, which is the representation
  // of an i32 value once promoted.  An operand that is a constant is
  // re-extended from its low word first: the type legalizer may have
  // zero-extended it, and a zero-extended 32-bit constant with bit 31 set
  // does not fit the LUI+ADDI materialiser.
  auto LowerBits = [&](unsigned Opcode) {
    auto Promoted = [&](SDValue V) {
      SDValue Ext = DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i64, V);
      if (auto *C = dyn_cast<ConstantSDNode>(Ext))
        return DAG.getConstant(C->getAPIntValue().trunc(32).sext(64), DL,
                               MVT::i64);
      return Ext;
    };
    SDValue LHS = Promoted(N->getOperand(0));
    SDValue RHS = Promoted(N->getOperand(1));
    SDValue Res = DAG.getNode(Opcode, DL, MVT::i64, LHS, RHS);
    Res = DAG.getNode(ISD::SIGN_EXTEND_INREG, DL, MVT::i64, Res,
                      DAG.getValueType(MVT::i32));
    Results.push_back(DAG.getNode(ISD::TRUNCATE, DL, N->getValueType(0), Res));
  };

  switch (N->getOpcode()) {
  case ISD::ADD:
    LowerW(MiniRVISD::ADDW);
    break;
  case ISD::SUB:
    LowerW(MiniRVISD::SUBW);
    break;
  case ISD::SHL:
    LowerW(MiniRVISD::SHLW);
    break;
  case ISD::SRL:
    LowerW(MiniRVISD::SRLW);
    break;
  case ISD::SRA:
    LowerW(MiniRVISD::SRAW);
    break;
  case ISD::MUL:
    LowerW(MiniRVISD::MULW);
    break;
  case ISD::SDIV:
    LowerW(MiniRVISD::DIVW);
    break;
  case ISD::UDIV:
    LowerW(MiniRVISD::DIVUW);
    break;
  case ISD::SREM:
    LowerW(MiniRVISD::REMW);
    break;
  case ISD::UREM:
    LowerW(MiniRVISD::REMUW);
    break;
  case ISD::AND:
    LowerBits(ISD::AND);
    break;
  case ISD::OR:
    LowerBits(ISD::OR);
    break;
  case ISD::XOR:
    LowerBits(ISD::XOR);
    break;
  default:
    llvm_unreachable("MiniRV does not custom-lower this i32 operation");
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

/// A select has to be built from arithmetic: the corpus has no conditional
/// move.  A comparison yields zero or one, so negating it produces an all-ones
/// mask for the true value, and the result is
///
///     false ^ ((true ^ false) & mask)
SDValue MiniRVTargetLowering::LowerSELECT_CC(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  assert(VT == getPointerTy(DAG.getDataLayout()) &&
         "select_cc is legalised after type legalisation");

  SDValue Cond = DAG.getNode(
      ISD::SETCC, DL, VT, Op.getOperand(0), Op.getOperand(1),
      DAG.getCondCode(cast<CondCodeSDNode>(Op.getOperand(4))->get()));
  SDValue TrueVal = Op.getOperand(2);
  SDValue FalseVal = Op.getOperand(3);

  SDValue Mask =
      DAG.getNode(ISD::SUB, DL, VT, DAG.getConstant(0, DL, VT), Cond);
  SDValue Diff = DAG.getNode(ISD::XOR, DL, VT, TrueVal, FalseVal);
  return DAG.getNode(ISD::XOR, DL, VT, FalseVal,
                     DAG.getNode(ISD::AND, DL, VT, Diff, Mask));
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
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  for (CCValAssign &VA : ArgLocs) {
    SDValue Arg;
    if (VA.isRegLoc()) {
      // Go through a virtual register so that the physical argument register
      // is recorded as live-in to the function.
      Register VReg = RegInfo.createVirtualRegister(&MiniRV::GPRRegClass);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      Arg = DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT());
      Chain = Arg.getValue(1);
    } else {
      assert(VA.isMemLoc() &&
             "argument is neither in a register nor on the stack");
      // Incoming stack arguments live above the return address, so a fixed
      // object addressed from the incoming stack pointer reads them.  The
      // calling convention assigns the register type, which is XLEN, so the
      // original IR type says how much of the slot is defined: a narrow
      // argument is read with LB/LH/LW, which also widens it to XLEN.
      unsigned Offset = VA.getLocMemOffset();
      const ISD::InputArg &IA = Ins[VA.getValNo()];
      EVT ValVT = IA.ArgVT;
      int FI;
      if (ValVT.isInteger() && ValVT.getSizeInBits() < 64) {
        FI = MF.getFrameInfo().CreateFixedObject(ValVT.getStoreSize(), Offset,
                                                 /*IsImmutable=*/true);
        SDValue FIN = DAG.getTargetFrameIndex(FI, PtrVT);
        ISD::LoadExtType Ext =
            IA.Flags.isZExt() ? ISD::ZEXTLOAD : ISD::SEXTLOAD;
        Arg = DAG.getExtLoad(Ext, DL, VA.getLocVT(), Chain, FIN,
                             MachinePointerInfo::getFixedStack(MF, FI), ValVT);
      } else {
        FI = MF.getFrameInfo().CreateFixedObject(VA.getLocVT().getStoreSize(),
                                                 Offset, /*IsImmutable=*/true);
        SDValue FIN = DAG.getTargetFrameIndex(FI, PtrVT);
        Arg = DAG.getLoad(VA.getLocVT(), DL, Chain, FIN,
                          MachinePointerInfo::getFixedStack(MF, FI));
      }
      Chain = Arg.getValue(1);
    }

    // The convention passes i8/i16/i32 values sign-extended in XLEN.
    if (Arg.getValueType() != VA.getValVT())
      Arg = DAG.getNode(ISD::TRUNCATE, DL, VA.getValVT(), Arg);
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
    SDValue Val = OutVals[I];
    if (Val.getValueType() != VA.getLocVT())
      Val = DAG.getNode(ISD::SIGN_EXTEND, DL, VA.getLocVT(), Val);
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), Val, Glue);
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

  if (IsVarArg)
    report_fatal_error("MiniRV does not support variadic calls");

  MachineFunction &MF = DAG.getMachineFunction();

  // Analyse the arguments: eight registers, the rest on the stack.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_MiniRV);

  // A tail call is a jump: the epilogue that PEI places before the terminator
  // releases the frame and restores the callee-saved registers, and the callee
  // returns straight to the caller's caller.  Only register arguments can be
  // forwarded, because a stack argument would have to be written to the area
  // this function received its own stack arguments in, which the epilogue is
  // about to give back.
  if (CLI.IsTailCall && CCInfo.getStackSize() != 0) {
    if (CLI.CB && CLI.CB->isMustTailCall())
      report_fatal_error(
          "MiniRV cannot tail call a function with stack arguments");
    CLI.IsTailCall = false;
  }

  // The outgoing argument area is reserved in the prologue, so this only
  // announces the maximum size to the frame machinery.
  unsigned NumBytes = CCInfo.getStackSize();
  if (NumBytes)
    Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, DL);

  // A direct callee becomes a target node so that the pattern can match it.
  // Anything else is an address that is already in a register and is called
  // indirectly.
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  bool IsIndirect = false;
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee =
        DAG.getTargetGlobalAddress(G->getGlobal(), DL, PtrVT, G->getOffset());
  else if (ExternalSymbolSDNode *S = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), PtrVT);
  else
    IsIndirect = true;

  // Stack arguments are written first, then the register arguments are
  // copied.  Keeping the stores on a TokenFactor (rather than chaining them
  // through the argument copies) matches the shape the scheduler expects.
  SDValue StackPtr;
  SmallVector<SDValue, 8> MemOpChains;
  SmallVector<std::tuple<Register, SDValue, EVT>, 8> RegsToPass;
  for (unsigned I = 0, E = ArgLocs.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    SDValue Val = OutVals[I];
    if (Val.getValueType() != VA.getLocVT())
      Val = DAG.getNode(ISD::SIGN_EXTEND, DL, VA.getLocVT(), Val);

    if (VA.isRegLoc()) {
      RegsToPass.emplace_back(VA.getLocReg(), Val, VA.getLocVT());
      continue;
    }

    assert(VA.isMemLoc() &&
           "argument is neither in a register nor on the stack");
    // With a reserved call frame the outgoing slots start at the current SP.
    if (!StackPtr.getNode())
      StackPtr = DAG.getCopyFromReg(Chain, DL, MiniRV::X2, PtrVT);
    SDValue Address =
        DAG.getNode(ISD::ADD, DL, PtrVT, StackPtr,
                    DAG.getIntPtrConstant(VA.getLocMemOffset(), DL));
    MemOpChains.push_back(
        DAG.getStore(Chain, DL, Val, Address,
                     MachinePointerInfo::getStack(MF, VA.getLocMemOffset())));
  }

  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);

  SDValue Glue;
  SmallVector<SDValue, 8> ArgRegs;
  for (auto &[Reg, Val, VT] : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, DL, Reg, Val, Glue);
    Glue = Chain.getValue(1);
    ArgRegs.push_back(DAG.getRegister(Reg, VT));
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

  // The tail call ends the block: there is no result to copy out and no call
  // sequence to close, because the callee's return goes to our caller.
  if (CLI.IsTailCall) {
    MF.getFrameInfo().setHasTailCall();
    if (Glue.getNode())
      Ops.push_back(Glue);
    return DAG.getNode(IsIndirect ? MiniRVISD::TAIL_REG : MiniRVISD::TAIL, DL,
                       MVT::Other, Ops);
  }

  // The glue goes last on a variadic call node.
  if (Glue.getNode())
    Ops.push_back(Glue);
  Chain = DAG.getNode(IsIndirect ? MiniRVISD::CALL_REG : MiniRVISD::CALL, DL,
                      NodeTys, Ops);
  Glue = Chain.getValue(1);

  // Release the reserved call frame.  The pseudo is erased by PEI because the
  // space was already allocated in the prologue.
  if (NumBytes) {
    Chain = DAG.getCALLSEQ_END(Chain, NumBytes, 0, Glue, DL);
    Glue = Chain.getValue(1);
  }

  // Copy the result out of x10.
  SmallVector<CCValAssign, 4> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(Ins, RetCC_MiniRV);
  for (CCValAssign &VA : RVLocs) {
    SDValue Ret =
        DAG.getCopyFromReg(Chain, DL, VA.getLocReg(), VA.getLocVT(), Glue);
    Chain = Ret.getValue(1);
    Glue = Ret.getValue(2);
    SDValue Val = Ret.getValue(0);
    if (Val.getValueType() != VA.getValVT())
      Val = DAG.getNode(ISD::TRUNCATE, DL, VA.getValVT(), Val);
    InVals.push_back(Val);
  }

  return Chain;
}

/// The *W target nodes all sign-extend bit 31 into the upper word, so the
/// DAG combiner can drop the explicit sign extension that an i32 value needs
/// after promotion.
unsigned MiniRVTargetLowering::ComputeNumSignBitsForTargetNode(
    SDValue Op, const APInt &DemandedElts, const SelectionDAG &DAG,
    unsigned Depth) const {
  switch (Op.getOpcode()) {
  case MiniRVISD::ADDW:
  case MiniRVISD::SUBW:
  case MiniRVISD::SHLW:
  case MiniRVISD::SRLW:
  case MiniRVISD::SRAW:
  case MiniRVISD::MULW:
  case MiniRVISD::DIVW:
  case MiniRVISD::DIVUW:
  case MiniRVISD::REMW:
  case MiniRVISD::REMUW:
    // Bits 63..31 are all equal to bit 31.
    return 33;
  default:
    return 1;
  }
}

const char *MiniRVTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case MiniRVISD::BRCC:
    return "MiniRVISD::BRCC";
  case MiniRVISD::CALL:
    return "MiniRVISD::CALL";
  case MiniRVISD::CALL_REG:
    return "MiniRVISD::CALL_REG";
  case MiniRVISD::TAIL:
    return "MiniRVISD::TAIL";
  case MiniRVISD::TAIL_REG:
    return "MiniRVISD::TAIL_REG";
  case MiniRVISD::RET:
    return "MiniRVISD::RET";
  case MiniRVISD::ADDW:
    return "MiniRVISD::ADDW";
  case MiniRVISD::SUBW:
    return "MiniRVISD::SUBW";
  case MiniRVISD::SHLW:
    return "MiniRVISD::SHLW";
  case MiniRVISD::SRLW:
    return "MiniRVISD::SRLW";
  case MiniRVISD::SRAW:
    return "MiniRVISD::SRAW";
  case MiniRVISD::MULW:
    return "MiniRVISD::MULW";
  case MiniRVISD::DIVW:
    return "MiniRVISD::DIVW";
  case MiniRVISD::DIVUW:
    return "MiniRVISD::DIVUW";
  case MiniRVISD::REMW:
    return "MiniRVISD::REMW";
  case MiniRVISD::REMUW:
    return "MiniRVISD::REMUW";
  default:
    return nullptr;
  }
}
