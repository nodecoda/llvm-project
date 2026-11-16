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
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "RVMinGenCallingConv.inc"

RVMinTargetLowering::RVMinTargetLowering(const TargetMachine &TM,
                                         const RVMinSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &RVMin::GPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  // Chapter 22: with A, the RV32A instructions (lr.w/sc.w/amo*.w) are exactly
  // what a 32-bit atomic needs.  Without A the size stays zero, which tells
  // AtomicExpand to lower every atomic to an `__atomic_*` libcall.
  setMaxAtomicSizeInBitsSupported(STI.hasStdExtA() ? 32 : 0);
  // Narrow atomics are widened to a word before the LL/SC loop is built.
  setMinCmpXchgSizeInBits(32);

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

  // Chapter 9: `(a + b)` on promoted `char`/`short` values ends in
  // `sign_extend_inreg`, because the result must be truncated back to the
  // narrow type.  RV32I has no such instruction, but two shifts do the job:
  // `srai(slli(x, 32-n), 32-n)`.  Letting the legalizer expand it keeps the
  // instruction selector free of special cases.
  //
  // Careful: SelectionDAG legalizes SIGN_EXTEND_INREG with the *inner* value
  // type (the operand of the VTSDNode), not with the result type.  Registering
  // the action for MVT::i32 therefore has no effect -- the queries are for
  // i1/i8/i16.
  // Chapter 21: with Zbb, i8/i16 sign extension is `sext.b`/`sext.h`, so the
  // node must survive to instruction selection.  i1 stays expanded: there is
  // no single-bit instruction in the subsets this course implements.
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);
  for (MVT VT : {MVT::i8, MVT::i16})
    setOperationAction(ISD::SIGN_EXTEND_INREG, VT,
                       STI.hasStdExtZbb() ? Legal : Expand);

  // Chapter 9: an `i1` (`_Bool`) is one byte in memory, exactly like a
  // `char`.  Promoting the memory type to i8 reuses the LB/LBU/SB patterns
  // instead of asking TableGen for an i1 load it cannot encode.
  setLoadExtAction(ISD::SEXTLOAD, MVT::i32, MVT::i1, Promote);
  setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, MVT::i1, Promote);
  setLoadExtAction(ISD::EXTLOAD, MVT::i32, MVT::i1, Promote);
  setTruncStoreAction(MVT::i32, MVT::i1, Promote);

  // Chapter 10: RV32I has no multiply, divide or remainder at all (that is
  // the M extension).  `Expand` here means "the legalizer emits a call to the
  // compiler runtime" -- the call support from chapter 8 makes that cheap:
  // MUL -> __mulsi3, SDIV -> __divsi3, UDIV -> __udivsi3, SREM -> __modsi3,
  // UREM -> __umodsi3.  Constant multiplies and division by powers of two are
  // folded into shifts and adds by the DAG combiner before this point.
  for (MVT VT : {MVT::i32}) {
    // RVMin has no instruction that produces two results, so these must be
    // expanded in both modes.  Without M the expansion reaches the libcalls;
    // with M it reaches MUL/MULH/DIV/REM, which is exactly what a 64-bit
    // multiply or a div/rem pair needs.
    setOperationAction(ISD::SMUL_LOHI, VT, Expand);
    setOperationAction(ISD::UMUL_LOHI, VT, Expand);
    setOperationAction(ISD::SDIVREM, VT, Expand);
    setOperationAction(ISD::UDIVREM, VT, Expand);

    // With the M extension every one of these is a single instruction with a
    // TableGen pattern, so the libcall path must not be taken.  Note the
    // `if`, not an early `break`: the bit-manipulation actions below must be
    // registered whether or not M is enabled.
    if (!STI.hasStdExtM()) {
      setOperationAction(ISD::MUL, VT, Expand);
      setOperationAction(ISD::MULHS, VT, Expand);
      setOperationAction(ISD::MULHU, VT, Expand);
      setOperationAction(ISD::SDIV, VT, Expand);
      setOperationAction(ISD::UDIV, VT, Expand);
      setOperationAction(ISD::SREM, VT, Expand);
      setOperationAction(ISD::UREM, VT, Expand);
    }

    // The bit-manipulation builtins (cpop/clz/ctz, rotates, byte swap) are
    // expanded into plain arithmetic sequences by the legalizer, not into
    // calls.  Chapter 21 makes the same operations legal when Zbb is enabled:
    // the patterns in RVMinInstrInfo.td then select one instruction each, and
    // the default subtarget keeps the expansion (and its output) unchanged.
    const TargetLowering::LegalizeAction BitAction =
        STI.hasStdExtZbb() ? Legal : Expand;
    setOperationAction(ISD::ROTL, VT, BitAction);
    setOperationAction(ISD::ROTR, VT, BitAction);
    setOperationAction(ISD::CTPOP, VT, BitAction);
    setOperationAction(ISD::CTLZ, VT, BitAction);
    setOperationAction(ISD::CTTZ, VT, BitAction);
    setOperationAction(ISD::BSWAP, VT, BitAction);

    // min/max only exist in Zbb; otherwise the legalizer expands them into
    // compares and selects.
    if (STI.hasStdExtZbb())
      setOperationAction({ISD::SMIN, ISD::SMAX, ISD::UMIN, ISD::UMAX}, VT,
                         Legal);
  }

  // Chapter 19: constant multiplies.  The generic combiner already rewrites
  // powers of two into shifts and, with M, division by a constant into a magic
  // multiply (MULHU/MULHS are legal).  The interesting case is `x*3`: the
  // target combine below turns it into `x + (x<<1)`, which works without M and
  // avoids both the constant materialization and the multiply.
  setTargetDAGCombine(ISD::MUL);

  // Chapter 10: soft float.  There is no f32/f64 register class, so the type
  // legalizer *softens* floating-point values into i32 pairs and rewrites
  // every FP operation as a libcall.  Nothing else is needed: __addsf3,
  // __muldf3, __ltsf2, __fixsfsi, ... fall out of the same machinery, and the
  // `ilp32` ABI already passes floats in integer registers.

  // Chapter 11 built `switch` as a comparison chain because RVMin had no
  // jump table: the switch builder only builds one when the target can lower
  // BR_JT *and* an indirect branch.  Chapter 18 provides both:
  //
  //   BR_JT   -> LW from an array of block addresses, then BRIND  (custom)
  //   BRIND   -> `jalr zero, 0(rs)`                               (legal)
  //
  // The table itself lives in .rodata: the generic AsmPrinter emits one
  // `.word <block>` per entry (EK_BlockAddress), and R_RISCV_32 already
  // resolves those to absolute addresses at link time.
  setOperationAction(ISD::BR_JT, MVT::Other, Custom);
  setOperationAction(ISD::BRIND, MVT::Other, Legal);

  // Chapter 9: 64-bit shifts.  RV32I shifts one register at a time, so the
  // legalizer would otherwise leave SHL_PARTS/SRL_PARTS/SRA_PARTS for the
  // selector.  Expanding them emits the standard compiler-runtime libcalls
  // (__ashldi3/__lshrdi3/__ashrdi3), which chapter 8 can already call.
  setOperationAction(ISD::SHL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRA_PARTS, MVT::i32, Expand);
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
  case ISD::BR_JT:
    return lowerBR_JT(Op, DAG);
  }
}

// Chapter 19: rewrite `x * C` into shifts and adds for the constants that
// have a cheap two-instruction form:
//
//   C = 2^k - 1   ->   (x << k) - x        x*3, x*7, x*15, ...
//   C = 2^k + 1   ->   (x << k) + x        x*5, x*9, x*17, ...
//
// Without M every constant multiply is a call to __mulsi3, so a constant with
// any two set bits is also worth three instructions:
//
//   C = 2^a + 2^b ->   (x << a) + (x << b) x*10, x*12, x*20, ...
//
// With M the last case is a pessimization (addi + mul is two instructions), so
// it is gated on M being absent.  Powers of two are already handled by the
// generic combiner and negative constants are left alone.
SDValue RVMinTargetLowering::PerformDAGCombine(SDNode *N,
                                               DAGCombinerInfo &DCI) const {
  if (N->getOpcode() != ISD::MUL)
    return SDValue();

  EVT VT = N->getValueType(0);
  if (VT != MVT::i32)
    return SDValue();

  SDValue X = N->getOperand(0);
  SDValue C = N->getOperand(1);
  if (!isa<ConstantSDNode>(C)) {
    if (!isa<ConstantSDNode>(X))
      return SDValue();
    std::swap(X, C);
  }

  const APInt &Imm = cast<ConstantSDNode>(C)->getAPIntValue();
  if (Imm.isNegative() || Imm.isPowerOf2() || Imm.isOne())
    return SDValue();

  SelectionDAG &DAG = DCI.DAG;
  SDLoc DL(N);
  auto shl = [&](unsigned K) {
    return DAG.getNode(ISD::SHL, DL, VT, X, DAG.getConstant(K, DL, VT));
  };

  APInt Plus1 = Imm + 1;
  if (Plus1.isPowerOf2())
    return DAG.getNode(ISD::SUB, DL, VT, shl(Plus1.logBase2()), X);

  APInt Minus1 = Imm - 1;
  if (Minus1.isPowerOf2())
    return DAG.getNode(ISD::ADD, DL, VT, shl(Minus1.logBase2()), X);

  if (!isOperationLegal(ISD::MUL, VT) && Imm.popcount() == 2) {
    unsigned Lo = Imm.countr_zero();
    unsigned Hi = Imm.getActiveBits() - 1;
    return DAG.getNode(ISD::ADD, DL, VT, shl(Lo), shl(Hi));
  }

  return SDValue();
}

// Chapter 19: the DAG combiner expands a division by a constant into a magic
// multiply, and a remainder into `x - (x/C)*C`.  With M that is usually the
// right trade-off, except when the function is optimized for size: then one
// `div`/`rem` is smaller than the sequence.  This mirrors what upstream
// RISC-V does (`RISCVTargetLowering::isIntDivCheap`).
static CallInst *emitAtomicAsm(IRBuilderBase &Builder, StringRef Asm,
                              ArrayRef<Type *> ArgTys, ArrayRef<Value *> Args,
                              StringRef Constraints) {
  FunctionType *FTy =
      FunctionType::get(Builder.getInt32Ty(), ArgTys, /*isVarArg=*/false);
  InlineAsm *IA = InlineAsm::get(FTy, Asm, Constraints,
                                 /*hasSideEffects=*/true);
  return Builder.CreateCall(IA, Args);
}

Value *RVMinTargetLowering::emitLoadLinked(IRBuilderBase &Builder,
                                           Type *ValueType, Value *Addr,
                                           AtomicOrdering Ord) const {
  assert(ValueType->isIntegerTy(32) && "RVMin A is a 32-bit LL/SC");
  Value *AddrInt = Builder.CreatePtrToInt(Addr, Builder.getInt32Ty());
  // aqrl is the conservative encoding: correct for every ordering.
  return emitAtomicAsm(Builder, "lr.w.aqrl $0, ($1)",
                       {Builder.getInt32Ty()}, {AddrInt}, "=r,r,~{memory}");
}

Value *RVMinTargetLowering::emitStoreConditional(IRBuilderBase &Builder,
                                                 Value *Val, Value *Addr,
                                                 AtomicOrdering Ord) const {
  Value *AddrInt = Builder.CreatePtrToInt(Addr, Builder.getInt32Ty());
  // sc.w returns 0 on success; AtomicExpand branches back while it is != 0.
  return emitAtomicAsm(Builder, "sc.w.aqrl $0, $2, ($1)",
                       {Builder.getInt32Ty(), Builder.getInt32Ty()},
                       {AddrInt, Val}, "=r,r,r,~{memory}");
}

std::pair<unsigned, const TargetRegisterClass *>
RVMinTargetLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, StringRef Constraint, MVT VT) const {
  if (Constraint == "r")
    return {0, &RVMin::GPRRegClass};
  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

TargetLowering::AtomicExpansionKind
RVMinTargetLowering::shouldExpandAtomicRMWInIR(const AtomicRMWInst *RMW) const {
  // These have a native AMO instruction and are matched in RVMinInstrInfo.td.
  // Every AMO is emitted in its .aqrl form, which is a valid (if
  // conservative) implementation of any ordering.
  // Only i32 has AMO instructions; everything narrower is widened to a word
  // by expandPartwordAtomicRMW and ends up in the LL/SC loop instead.
  if (!RMW->getType()->isIntegerTy(32))
    return AtomicExpansionKind::CmpXChg;

  switch (RMW->getOperation()) {
  case AtomicRMWInst::Add:
  case AtomicRMWInst::And:
  case AtomicRMWInst::Or:
  case AtomicRMWInst::Xor:
  case AtomicRMWInst::Xchg:
  case AtomicRMWInst::Max:
  case AtomicRMWInst::Min:
  case AtomicRMWInst::UMax:
  case AtomicRMWInst::UMin:
    return AtomicExpansionKind::None;
  default:
    // No AMO for sub/nand/smin/smax: let AtomicExpand build a cmpxchg loop,
    // which shouldExpandAtomicCmpXchgInIR then turns into lr.w/sc.w.
    return AtomicExpansionKind::CmpXChg;
  }
}

bool RVMinTargetLowering::isIntDivCheap(EVT VT, AttributeList Attr) const {
  return VT == MVT::i32 && Attr.hasFnAttr(Attribute::MinSize) &&
         isOperationLegal(ISD::SDIV, VT);
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

// `switch` with many cases: the DAG builder hands us the jump table index and
// a normalized 0-based selector.  Lowering is three steps:
//
//   lui/addi  t, %hi(.LJTI0)       address of the table (R_RISCV_HI20/LO12)
//   slli      i, index, 2          one 4-byte entry per case
//   add       t, t, i
//   lw        t, 0(t)
//   jalr      zero, 0(t)           BRIND
//
// The entries themselves are emitted by the generic AsmPrinter from the jump
// table info installed below; the DAG only sees the load and the branch.
SDValue RVMinTargetLowering::lowerBR_JT(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Table = Op.getOperand(1);
  SDValue Index = Op.getOperand(2);
  SDLoc DL(Op);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  MachineFunction &MF = DAG.getMachineFunction();

  auto *JT = cast<JumpTableSDNode>(Table);
  SDValue TableAddr = DAG.getTargetJumpTable(JT->getIndex(), PtrVT);

  // Entries are 4 bytes wide; the selector is already 0-based.
  SDValue Scale = DAG.getConstant(2, DL, PtrVT);
  SDValue Offset = DAG.getNode(ISD::SHL, DL, PtrVT, Index, Scale);
  SDValue EntryAddr = DAG.getNode(ISD::ADD, DL, PtrVT, TableAddr, Offset);

  SDValue Entry = DAG.getLoad(PtrVT, DL, Chain, EntryAddr,
                              MachinePointerInfo::getJumpTable(MF));
  return DAG.getNode(ISD::BRIND, DL, MVT::Other, Entry.getValue(1), Entry);
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
  case RVMinISD::TAIL:
    return "RVMinISD::TAIL";
  default:
    return nullptr;
  }
}

bool RVMinTargetLowering::mayBeEmittedAsTailCall(const CallInst *CI) const {
  // RVMin can emit a real `j` for a call in tail position (see LowerCall), so
  // every `tail` marker survives; whether it is actually in tail position is
  // decided by SelectionDAGBuilder::isInTailCallPosition.
  return CI->isTailCall();
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

  // Chapter 17: a tail call reuses the caller's frame.  After the epilogue sp
  // is back at entry_sp, which is exactly where a normal call would put the
  // outgoing argument area, and callee-saved registers are restored, so the
  // callee can return straight to our caller.  That only works when nothing
  // has to be written into the (already gone) frame: register arguments only,
  // no varargs, no dynamic objects.
  bool IsTailCall = CLI.IsTailCall;
  if (IsTailCall &&
      (NumBytes != 0 || MF.getFrameInfo().hasVarSizedObjects())) {
    // `tail` is only a hint, so fall back to a normal call + return.  A
    // `musttail` call has to be diagnosable: the frontend promised that no
    // frame is needed after it.
    if (CLI.CB && CLI.CB->isMustTailCall())
      MF.getFunction().getContext().diagnose(DiagnosticInfoUnsupported{
          MF.getFunction(),
          "RVMin: musttail requires a fixed frame and arguments that fit in "
          "registers"});
    IsTailCall = false;
  }
  CLI.IsTailCall = IsTailCall;

  // A tail call has no call sequence: there is no argument area to reserve and
  // no return to collect.
  if (!IsTailCall)
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

  if (IsTailCall) {
    // The epilogue (restores + `sp += N`) is emitted in this block because
    // PseudoTAIL is a return; all that is left is the jump.
    MF.getFrameInfo().setHasTailCall();
    return DAG.getNode(RVMinISD::TAIL, DL, MVT::Other, Ops);
  }

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
