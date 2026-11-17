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
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

// F and D have ordered EQ/LT/LE only.  Every other condition is expanded into
// one of those plus an XOR or an operand swap, so `x != y` and friends still
// work without extra instructions.  Shared by the f32 and f64 blocks below.
static const ISD::CondCode FPCCToExpand[] = {
    ISD::SETOGT, ISD::SETOGE, ISD::SETONE, ISD::SETUEQ, ISD::SETUGT,
    ISD::SETUGE, ISD::SETULT, ISD::SETULE, ISD::SETUNE, ISD::SETGT,
    ISD::SETGE,  ISD::SETNE,  ISD::SETO,   ISD::SETUO};

// Chapter 51: the calling-convention table is static, but the ABI is a module
// property.  A module compiled for `ilp32` passes floating values as their
// integer bit patterns even when the instructions are hardware F/D.
static bool RVMinIsSoftFloatABI(const CCState &State) {
  const MachineFunction &MF = State.getMachineFunction();
  const Module *M = MF.getFunction().getParent();
  StringRef ABI = MF.getTarget().getTargetABIName(*M);
  if (!ABI.empty())
    return ABI == "ilp32";

  // No module flag: getABI() derives ilp32f/ilp32d from the ISA, so only the
  // no-F case is soft.
  const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();
  return !STI.hasStdExtF() && !STI.hasStdExtD();
}

// Chapter 55: the vector calling convention is only meaningful when V is
// enabled.  The CC table uses this predicate to avoid assigning a v4i32 value
// to V8 in a scalar-only module.
static bool RVMinHasStdExtV(const CCState &State) {
  return State.getMachineFunction()
      .getSubtarget<RVMinSubtarget>()
      .hasStdExtV();
}

static bool isSupportedVectorABIType(EVT VT) { return VT == MVT::v4i32; }
static bool isLMUL2VectorABIType(EVT VT) { return VT == MVT::v8i32; }

static bool isSupportedVectorABIType(const Type *Ty) {
  auto *VT = dyn_cast<FixedVectorType>(Ty);
  return VT && VT->getNumElements() == 4 &&
         VT->getElementType()->isIntegerTy(32);
}

static bool isLMUL2VectorABIType(const Type *Ty) {
  auto *VT = dyn_cast<FixedVectorType>(Ty);
  return VT && VT->getNumElements() == 8 &&
         VT->getElementType()->isIntegerTy(32);
}

// Chapter 51: an f64 in the soft-float ABI occupies two i32 locations.  The
// pair must start at an even register (a0/a1, a2/a3, ...), matching the RISC-V
// ILP32 convention; if no pair is free, the whole double goes to an 8-byte
// stack slot.  The lowering code splits the value through a private stack
// object because RV32D has no fmv.x.d.
static bool RVMinCC_AssignF64AsTwoI32(unsigned &ValNo, MVT &ValVT,
                                      MVT &LocVT,
                                      CCValAssign::LocInfo &LocInfo,
                                      ISD::ArgFlagsTy &ArgFlags,
                                      CCState &State) {
  static const MCPhysReg Pairs[][2] = {
      {RVMin::X10, RVMin::X11}, {RVMin::X12, RVMin::X13},
      {RVMin::X14, RVMin::X15}, {RVMin::X16, RVMin::X17}};

  for (const auto &Pair : Pairs) {
    if (State.isAllocated(Pair[0]) || State.isAllocated(Pair[1]))
      continue;
    MCRegister Lo = State.AllocateReg(Pair[0]);
    MCRegister Hi = State.AllocateReg(Pair[1]);
    assert(Lo && Hi && "pair was free a moment ago");
    State.addLoc(CCValAssign::getCustomReg(ValNo, ValVT, Lo, MVT::i32,
                                           CCValAssign::BCvt));
    State.addLoc(CCValAssign::getCustomReg(ValNo, ValVT, Hi, MVT::i32,
                                           CCValAssign::BCvt));
    return true;
  }

  int64_t Offset = State.AllocateStack(8, Align(8));
  State.addLoc(CCValAssign::getCustomMem(ValNo, ValVT, Offset, MVT::f64,
                                         CCValAssign::Full));
  return true;
}

// Chapter 51: a double return always uses a0/a1 in ILP32.  There is only one
// return value, so failing to get both registers means the convention cannot
// lower the value; return false and let CheckReturn reject it.
static bool RVMinCC_AssignF64Return(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                    CCValAssign::LocInfo &LocInfo,
                                    ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  if (State.isAllocated(RVMin::X10) || State.isAllocated(RVMin::X11))
    return false;
  MCRegister Lo = State.AllocateReg(RVMin::X10);
  MCRegister Hi = State.AllocateReg(RVMin::X11);
  if (!Lo || !Hi)
    return false;
  State.addLoc(CCValAssign::getCustomReg(ValNo, ValVT, Lo, MVT::i32,
                                         CCValAssign::BCvt));
  State.addLoc(CCValAssign::getCustomReg(ValNo, ValVT, Hi, MVT::i32,
                                         CCValAssign::BCvt));
  return true;
}

#define GET_CALLING_CONV_IMPL
#include "RVMinGenCallingConv.inc"

RVMinTargetLowering::RVMinTargetLowering(const TargetMachine &TM,
                                         const RVMinSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &RVMin::GPRRegClass);
  // Chapter 40: F makes f32 a legal value type, so it stays in an FPR instead
  // of being softened into an i32 pair.  This must happen before
  // computeRegisterProperties(), which derives the type actions from the set
  // of register classes.
  if (STI.hasStdExtF())
    addRegisterClass(MVT::f32, &RVMin::FPR32RegClass);
  // Chapter 41: with D, f64 is legal and lives in the 64-bit view of the same
  // register file.  D implies F, so both classes are registered together.
  if (STI.hasStdExtD())
    addRegisterClass(MVT::f64, &RVMin::FPR64RegClass);
  // Chapter 48: with V, a fixed <4 x i32> (128 bits) is the native vector
  // value and lives in one VR.  This must also happen before
  // computeRegisterProperties() so the type action is Legal, not Expand.
  if (STI.hasStdExtV()) {
    addRegisterClass(MVT::v4i32, &RVMin::VRRegClass);
    // Chapter 52: mask values are legal too; v4i1 lives in the same VR file.
    addRegisterClass(MVT::v4i1, &RVMin::VRRegClass);
    // Chapter 53: LMUL=2 gives <8 x i32> a 256-bit register pair.
    addRegisterClass(MVT::v8i32, &RVMin::VRM2RegClass);
    // Chapter 54: with F as well, <4 x float> shares the 128-bit VR file.
    if (STI.hasStdExtF())
      addRegisterClass(MVT::v4f32, &RVMin::VRRegClass);
  }
  computeRegisterProperties(STI.getRegisterInfo());

  if (STI.hasStdExtV()) {
    // Start from a clean slate: every vector operation this chapter does not
    // implement is expanded by the legalizer instead of reaching instruction
    // selection without a pattern.  The six integer ALU operations below are
    // the ones with a direct .vv instruction.
    for (unsigned Op = 0; Op < ISD::BUILTIN_OP_END; ++Op)
      setOperationAction(Op, MVT::v4i32, Expand);
    setOperationAction({ISD::ADD, ISD::SUB, ISD::MUL, ISD::AND, ISD::OR,
                        ISD::XOR},
                       MVT::v4i32, Legal);
    setOperationAction(ISD::LOAD, MVT::v4i32, Legal);
    setOperationAction(ISD::STORE, MVT::v4i32, Legal);
    setOperationAction(ISD::BUILD_VECTOR, MVT::v4i32, Custom);
    setOperationAction(ISD::SPLAT_VECTOR, MVT::v4i32, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4i32, Custom);
    // Chapter 49: `llvm.vector.reduce.add` has a direct vredsum.vs sequence.
    setOperationAction(ISD::VECREDUCE_ADD, MVT::v4i32, Custom);
    // Chapter 52: vselect -> vmerge.vvm with the mask copied into v0.  The
    // mask type v4i1 is legal (it lives in VR), so the type legalizer leaves
    // the node alone instead of expanding it through scalar selects.
    setOperationAction(ISD::VSELECT, MVT::v4i32, Legal);
    setOperationAction(ISD::BUILD_VECTOR, MVT::v4i1, Custom);

    // Chapter 53: the same clean slate for the LMUL=2 type.  Only add and
    // memory are selected directly; everything else expands instead of
    // reaching instruction selection without a pattern.
    for (unsigned Op = 0; Op < ISD::BUILTIN_OP_END; ++Op)
      setOperationAction(Op, MVT::v8i32, Expand);
    setOperationAction(ISD::ADD, MVT::v8i32, Legal);
    setOperationAction(ISD::LOAD, MVT::v8i32, Legal);
    setOperationAction(ISD::STORE, MVT::v8i32, Legal);

    // Chapter 54: vector float.  Only add/sub/mul and memory are selected;
    // splat/extract use VFMV_V_F/VFMV_F_S, everything else expands.
    if (STI.hasStdExtF()) {
      for (unsigned Op = 0; Op < ISD::BUILTIN_OP_END; ++Op)
        setOperationAction(Op, MVT::v4f32, Expand);
      setOperationAction({ISD::FADD, ISD::FSUB, ISD::FMUL}, MVT::v4f32,
                         Legal);
      setOperationAction(ISD::LOAD, MVT::v4f32, Legal);
      setOperationAction(ISD::STORE, MVT::v4f32, Legal);
      setOperationAction(ISD::BUILD_VECTOR, MVT::v4f32, Custom);
      setOperationAction(ISD::SPLAT_VECTOR, MVT::v4f32, Custom);
      setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4f32, Custom);
    }
  }

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

  // Chapter 10: soft float.  Without the F extension there is no f32/f64
  // register class, so the type legalizer *softens* floating-point values into
  // i32 pairs and rewrites every FP operation as a libcall.  Nothing else is
  // needed: __addsf3, __muldf3, __ltsf2, __fixsfsi, ... fall out of the same
  // machinery, and the `ilp32` ABI already passes floats in integer registers.
  //
  // Chapter 40: with F, f32 is legal and the single-instruction patterns take
  // over.  Only the operations that genuinely have no F instruction are
  // expanded or turned into libcalls.
  if (STI.hasStdExtF()) {
    // These default to Expand in TargetLoweringBase even for legal types.
    setOperationAction({ISD::FMINNUM, ISD::FMAXNUM}, MVT::f32, Legal);
    setOperationAction(ISD::FREM, MVT::f32, LibCall);

    setCondCodeAction(FPCCToExpand, MVT::f32, Expand);

    // A value select on f32 has no instruction.  It is lowered by
    // bitcasting both arms to i32 and reusing the integer select sequence;
    // SELECT must therefore be Custom, not Expand (the legalizer refuses to
    // expand SELECT_CC when SELECT is also expanded).  Branches on f32
    // conditions likewise go through a GPR.
    setOperationAction(ISD::SELECT, MVT::f32, Custom);
    setOperationAction(ISD::SELECT_CC, MVT::f32, Expand);
    setOperationAction(ISD::BR_CC, MVT::f32, Expand);

    // Rounding and transcendental operations are not in the minimal F subset.
    setOperationAction({ISD::FCEIL, ISD::FFLOOR, ISD::FTRUNC, ISD::FRINT,
                        ISD::FROUND, ISD::FROUNDEVEN},
                       MVT::f32, Expand);
    setOperationAction({ISD::FSIN, ISD::FCOS, ISD::FSINCOS, ISD::FPOW},
                       MVT::f32, Expand);
  }

  // Chapter 41: the double-precision half.  Same shape as F; the differences
  // are the 64-bit calls, the float<->double conversions, and the fact that a
  // double cannot be moved through the integer register file on RV32.
  if (STI.hasStdExtD()) {
    setOperationAction({ISD::FMINNUM, ISD::FMAXNUM}, MVT::f64, Legal);
    setOperationAction(ISD::FREM, MVT::f64, LibCall);

    setCondCodeAction(FPCCToExpand, MVT::f64, Expand);

    setOperationAction(ISD::SELECT, MVT::f64, Custom);
    setOperationAction(ISD::SELECT_CC, MVT::f64, Expand);
    setOperationAction(ISD::BR_CC, MVT::f64, Expand);

    setOperationAction({ISD::FCEIL, ISD::FFLOOR, ISD::FTRUNC, ISD::FRINT,
                        ISD::FROUND, ISD::FROUNDEVEN},
                       MVT::f64, Expand);
    setOperationAction({ISD::FSIN, ISD::FCOS, ISD::FSINCOS, ISD::FPOW},
                       MVT::f64, Expand);

    // fcvt.d.s / fcvt.s.d are single instructions.  Without these the
    // legalizer would call __extendsfdf2/__truncdfsf2 and the codegen test
    // would never see FCVT_D_S/FCVT_S_D.
    setOperationAction(ISD::FP_EXTEND, MVT::f64, Legal);
    setOperationAction(ISD::FP_ROUND, MVT::f32, Legal);

    // `float x = (float)d;` reaches the DAG as a truncating store: the
    // combiner folds FP_ROUND into the store because the generic legality
    // rules say f64->f32 truncation is fine.  RVMin has no truncstore
    // pattern (only plain 4- and 8-byte stores), so expand it back into
    // FP_ROUND + FSW, which the selector does know.
    setTruncStoreAction(MVT::f64, MVT::f32, Expand);

    // The mirror image: `(double)f` loads an f32 slot with an extending
    // load once the combiner folds FP_EXTEND into it.  Expand to FLW +
    // FCVT_D_S, again the two instructions the selector knows.
    setLoadExtAction(ISD::EXTLOAD, MVT::f64, MVT::f32, Expand);

    // No register-pair move exists, so the immediate is lowered by
    // lowerConstantFP through a stack slot.
    setOperationAction(ISD::ConstantFP, MVT::f64, Custom);
  }

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

RVMinTargetLowering::FPImmAction
RVMinTargetLowering::getFPImmAction(const APFloat &Imm, EVT VT,
                                    bool ForCodeSize) const {
  if (VT == MVT::f32 && isTypeLegal(VT))
    return FPImmAction::Bitcast;
  return FPImmAction::ConstantPool;
}

//===----------------------------------------------------------------------===//
// Chapter 40: ABI selection and validation
//===----------------------------------------------------------------------===//
//
// The ISA decides which instructions are available; the ABI decides where
// values cross a function boundary.  Keeping the two separate is what makes
// `-mabi=ilp32 -march=rv32if` a meaningful (if unimplemented) request rather
// than a silent contradiction.

RVMinABI RVMinTargetLowering::getABI(const MachineFunction &MF) const {
  const Module *M = MF.getFunction().getParent();
  StringRef Name = getTargetMachine().getTargetABIName(*M);

  if (Name.empty()) {
    const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();
    if (STI.hasStdExtD())
      return RVMinABI::ILP32D;
    if (STI.hasStdExtF())
      return RVMinABI::ILP32F;
    return RVMinABI::ILP32;
  }

  if (Name == "ilp32")
    return RVMinABI::ILP32;
  if (Name == "ilp32f")
    return RVMinABI::ILP32F;
  if (Name == "ilp32d")
    return RVMinABI::ILP32D;
  report_fatal_error(Twine("RVMin: unknown ABI '") + Name +
                         "' (expected ilp32, ilp32f or ilp32d)",
                     /*gen_crash_diag=*/false);
}

void RVMinTargetLowering::checkABICompatibility(
    const MachineFunction &MF) const {
  const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();

  switch (getABI(MF)) {
  case RVMinABI::ILP32:
    // Chapter 51: a soft-float ABI with hardware F/D is legal RISC-V.  Values
    // cross calls as integer bit patterns (f32 in one GPR, f64 in an even
    // GPR pair or an 8-byte stack slot) while arithmetic uses the F/D
    // instructions.  The conversion is implemented in LowerCall,
    // LowerFormalArguments and LowerReturn.
    break;
  case RVMinABI::ILP32F:
    if (!STI.hasStdExtF())
      report_fatal_error(
          "RVMin: the ilp32f ABI requires the F extension", false);
    // With D, f64 becomes legal and would cross calls in fa0-fa7, which is
    // the ilp32d convention.  Mixing the two silently would miscompile, so
    // this combination is rejected until it is implemented on purpose.
    if (STI.hasStdExtD())
      report_fatal_error(
          "RVMin: the ilp32f ABI with the D extension is not implemented; "
          "use -mabi=ilp32d or disable D with -mattr=-d",
          /*gen_crash_diag=*/false);
    break;
  case RVMinABI::ILP32D:
    if (!STI.hasStdExtD())
      report_fatal_error(
          "RVMin: the ilp32d ABI requires the D extension",
          /*gen_crash_diag=*/false);
    break;
  }
}

SDValue RVMinTargetLowering::LowerOperation(SDValue Op,
                                            SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  default:
    llvm_unreachable("RVMin: unhandled custom lowering");
  case ISD::SELECT:
    return lowerSELECT(Op, DAG);
  case ISD::SELECT_CC:
    return lowerSELECT_CC(Op, DAG);
  case ISD::BRCOND:
    return lowerBRCOND(Op, DAG);
  case ISD::BR_CC:
    return lowerBR_CC(Op, DAG);
  case ISD::BR_JT:
    return lowerBR_JT(Op, DAG);
  case ISD::ConstantFP:
    return lowerConstantFP(Op, DAG);
  case ISD::BUILD_VECTOR:
    if (Op.getValueType() == MVT::v4i1)
      return lowerMaskBUILD_VECTOR(Op, DAG);
    return lowerBUILD_VECTOR(Op, DAG);
  case ISD::SPLAT_VECTOR:
    return lowerSPLAT_VECTOR(Op, DAG);
  case ISD::EXTRACT_VECTOR_ELT:
    return lowerEXTRACT_VECTOR_ELT(Op, DAG);
  case ISD::VECREDUCE_ADD:
    return lowerVECREDUCE_ADD(Op, DAG);
  }
}

// Chapter 52: a <4 x i1> constant is a 4-bit mask.  RVV reads element i's
// mask bit from bit i of v0, so broadcasting the integer with vmv.v.x gives
// the right image.  Non-constant masks are intentionally not handled yet:
// vector compares arrive with the SETCC chapter, and a scalar i1 splat would
// need a different materialization.
SDValue
RVMinTargetLowering::lowerMaskBUILD_VECTOR(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDLoc DL(Op);
  uint32_t Bits = 0;
  for (unsigned I = 0; I != 4; ++I) {
    auto *C = dyn_cast<ConstantSDNode>(Op.getOperand(I));
    if (!C)
      report_fatal_error(
          "RVMin: non-constant vector masks are not supported yet", false);
    if (C->getZExtValue() & 1)
      Bits |= 1u << I;
  }
  SDValue Mask = DAG.getConstant(Bits, DL, MVT::i32);
  return SDValue(
      DAG.getMachineNode(RVMin::VMV_V_X, DL, MVT::v4i1, Mask), 0);
}

// Chapter 48: materialize four i32 elements through a private 16-byte stack
// object, then read them back as one v4i32.  The vector load is selected by
// RVMinDAGToDAGISel as VLE32_V.  The slot never escapes, so the stores and
// load form their own chain and cannot alias a user pointer.
SDValue RVMinTargetLowering::lowerVectorThroughStack(
    ArrayRef<SDValue> Elems, const SDLoc &DL, SelectionDAG &DAG,
    MVT VecVT) const {
  assert(Elems.size() == 4 && "RVMin vectors are exactly four elements");
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  int FI = MFI.CreateStackObject(16, Align(16), /*isSpillSlot=*/false);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue FIN = DAG.getFrameIndex(FI, PtrVT);
  // The slot is private and never escapes, so it does not need to be ordered
  // against the current root.  Chaining from the entry node is also required
  // for correctness: while lowering a BUILD_VECTOR that feeds a store, the
  // root already contains that store, and starting from getRoot() would make
  // the new load (and therefore the stored value) depend on its own user.
  SDValue Chain = DAG.getEntryNode();
  for (unsigned I = 0; I != 4; ++I) {
    SDValue Addr = DAG.getNode(ISD::ADD, DL, PtrVT, FIN,
                               DAG.getConstant(I * 4, DL, PtrVT));
    Chain = DAG.getStore(
        Chain, DL, Elems[I], Addr,
        MachinePointerInfo::getFixedStack(MF, FI, I * 4), Align(4));
  }
  return DAG.getLoad(VecVT, DL, Chain, FIN,
                     MachinePointerInfo::getFixedStack(MF, FI), Align(16));
}

SDValue RVMinTargetLowering::lowerBUILD_VECTOR(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc DL(Op);
  assert(Op.getNumOperands() == 4 && "RVMin vectors are exactly four elements");

  // Chapter 54: a <4 x float> splat goes straight to vfmv.v.f.  Four
  // identical operands are the common shape produced for a scalar broadcast;
  // other shapes go through the same private stack slot as the integer path.
  if (Op.getValueType() == MVT::v4f32) {
    bool AllIdentical = true;
    for (unsigned I = 1; I != 4; ++I)
      if (Op.getOperand(I) != Op.getOperand(0)) {
        AllIdentical = false;
        break;
      }
    if (AllIdentical)
      return SDValue(DAG.getMachineNode(RVMin::VFMV_V_F, DL, MVT::v4f32,
                                        Op.getOperand(0)),
                     0);
    SmallVector<SDValue, 4> Elems(Op->op_begin(), Op->op_end());
    return lowerVectorThroughStack(Elems, DL, DAG, MVT::v4f32);
  }

  // A build_vector whose four elements are the same constant is the natural
  // spelling of a splat.  VMV_V_I covers the 5-bit signed immediate range;
  // larger values reuse the stack path below.
  auto *C0 = dyn_cast<ConstantSDNode>(Op.getOperand(0));
  bool AllSame = C0 != nullptr;
  if (AllSame) {
    int64_t Value = C0->getSExtValue();
    for (unsigned I = 1; I != 4; ++I) {
      auto *C = dyn_cast<ConstantSDNode>(Op.getOperand(I));
      if (!C || C->getSExtValue() != Value) {
        AllSame = false;
        break;
      }
    }
    if (AllSame && isInt<5>(Value))
      return SDValue(
          DAG.getMachineNode(RVMin::VMV_V_I, DL, MVT::v4i32,
                             DAG.getTargetConstant(Value, DL, MVT::i32)),
          0);
  }

  // Four identical non-constant elements are a splat too; broadcast the
  // scalar with vmv.v.x instead of materializing it through the stack.
  bool AllIdentical = true;
  for (unsigned I = 1; I != 4; ++I)
    if (Op.getOperand(I) != Op.getOperand(0)) {
      AllIdentical = false;
      break;
    }
  if (AllIdentical)
    return SDValue(
        DAG.getMachineNode(RVMin::VMV_V_X, DL, MVT::v4i32, Op.getOperand(0)),
        0);

  SmallVector<SDValue, 4> Elems(Op->op_begin(), Op->op_end());
  return lowerVectorThroughStack(Elems, DL, DAG, MVT::v4i32);
}

SDValue RVMinTargetLowering::lowerSPLAT_VECTOR(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Val = Op.getOperand(0);
  if (Op.getValueType() == MVT::v4f32)
    return SDValue(
        DAG.getMachineNode(RVMin::VFMV_V_F, DL, MVT::v4f32, Val), 0);
  if (auto *C = dyn_cast<ConstantSDNode>(Val)) {
    int64_t Value = C->getSExtValue();
    if (isInt<5>(Value))
      return SDValue(
          DAG.getMachineNode(RVMin::VMV_V_I, DL, MVT::v4i32,
                             DAG.getTargetConstant(Value, DL, MVT::i32)),
          0);
  }
  // Chapter 49: a variable splat is `vmv.v.x`, not four stores and a vector
  // reload.  Only the constant immediate case keeps vmv.v.i.
  return SDValue(DAG.getMachineNode(RVMin::VMV_V_X, DL, MVT::v4i32, Val), 0);
}

SDValue RVMinTargetLowering::lowerEXTRACT_VECTOR_ELT(SDValue Op,
                                                     SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Vec = Op.getOperand(0);
  auto *C = dyn_cast<ConstantSDNode>(Op.getOperand(1));
  if (!C)
    report_fatal_error(
        "RVMin: variable vector extract is not supported yet", false);
  int64_t Index = C->getSExtValue();
  if (Index < 0 || Index >= 4)
    report_fatal_error("RVMin: vector extract index out of range", false);

  // Chapter 54: f32 extraction uses the FP scalar move; the stack fallback
  // loads an f32 instead of an i32.
  bool IsF32 = Vec.getValueType() == MVT::v4f32;
  MVT ElemVT = IsF32 ? MVT::f32 : MVT::i32;

  // Element 0 has a direct vector-to-scalar move.  The other constant indices
  // spill the vector once and load the requested element, which is also a
  // useful exercise of the vector store path.
  if (Index == 0) {
    unsigned Opc = IsF32 ? RVMin::VFMV_F_S : RVMin::VMV_X_S;
    return SDValue(DAG.getMachineNode(Opc, DL, ElemVT, Vec), 0);
  }

  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  int FI = MFI.CreateStackObject(16, Align(16), /*isSpillSlot=*/false);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue FIN = DAG.getFrameIndex(FI, PtrVT);
  // See lowerVectorThroughStack: the slot is private, so the entry node is a
  // safe chain start and avoids a cycle through the current root.
  SDValue Chain = DAG.getStore(
      DAG.getEntryNode(), DL, Vec, FIN,
      MachinePointerInfo::getFixedStack(MF, FI, 0), Align(16));
  SDValue Addr = DAG.getNode(ISD::ADD, DL, PtrVT, FIN,
                             DAG.getConstant(Index * 4, DL, PtrVT));
  return DAG.getLoad(ElemVT, DL, Chain, Addr,
                     MachinePointerInfo::getFixedStack(MF, FI, Index * 4),
                     Align(4));
}

// Chapter 49: reduce <4 x i32> to i32.  vredsum.vs adds the four elements of
// vs2 to element 0 of vs1, so seed vs1 with zero and move the result out of
// the vector register.  The three instructions are all in the Zve32x subset
// already used by chapters 47/48.
SDValue RVMinTargetLowering::lowerVECREDUCE_ADD(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue Vec = Op.getOperand(0);
  SDValue Zero = DAG.getTargetConstant(0, DL, MVT::i32);
  SDValue Seed = SDValue(
      DAG.getMachineNode(RVMin::VMV_V_I, DL, MVT::v4i32, Zero), 0);
  SDValue Sum = SDValue(
      DAG.getMachineNode(RVMin::VREDSUM_VS, DL, MVT::v4i32, Vec, Seed), 0);
  return SDValue(DAG.getMachineNode(RVMin::VMV_X_S, DL, MVT::i32, Sum), 0);
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

// BRCOND always carries a boolean in the DAG.  When it came from an integer
// setcc we can branch on the original comparison operands directly; otherwise
// compare the boolean against zero.
//
// Chapter 40: a floating-point setcc must take the second path.  F has no
// conditional branch on FP values, so the comparison stays a `feq.s`/`flt.s`
// (or its expanded form) that produces 0/1 in a GPR, and the branch tests that
// GPR.  Branching on the FP operands directly would leave an unselectable
// BR_CC node.
SDValue RVMinTargetLowering::lowerBRCOND(SDValue Op,
                                         SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Cond = Op.getOperand(1);
  SDValue Dest = Op.getOperand(2);
  SDLoc DL(Op);

  if (Cond.getOpcode() == ISD::SETCC &&
      Cond.getOperand(0).getSimpleValueType() == MVT::i32) {
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
// Chapter 40: `select c, t, f` on f32.  Both arms are just 32-bit patterns,
// so bitcast them to i32, use the integer select (a branchless mask sequence
// built by lowerSELECT_CC), and bitcast the result back.
SDValue RVMinTargetLowering::lowerSELECT(SDValue Op,
                                         SelectionDAG &DAG) const {
  SDValue Cond = Op.getOperand(0);
  SDValue TrueV = Op.getOperand(1);
  SDValue FalseV = Op.getOperand(2);
  SDLoc DL(Op);

  // Chapter 41: a double cannot be bitcast to a legal integer type (i64 is
  // expanded and there is no fmv.x.d on RV32D).  Select the address of the
  // arm instead: store both to private slots and reload through the winner.
  if (Op.getValueType() == MVT::f64) {
    MachineFunction &MF = DAG.getMachineFunction();
    MachineFrameInfo &MFI = MF.getFrameInfo();
    EVT PtrVT = getPointerTy(DAG.getDataLayout());

    int TrueFI = MFI.CreateStackObject(8, Align(8), /*isSpillSlot=*/false);
    int FalseFI = MFI.CreateStackObject(8, Align(8), /*isSpillSlot=*/false);
    SDValue TrueAddr = DAG.getFrameIndex(TrueFI, PtrVT);
    SDValue FalseAddr = DAG.getFrameIndex(FalseFI, PtrVT);

    SDValue Chain = DAG.getEntryNode();
    Chain = DAG.getStore(Chain, DL, TrueV, TrueAddr,
                         MachinePointerInfo::getFixedStack(MF, TrueFI),
                         Align(8));
    Chain = DAG.getStore(Chain, DL, FalseV, FalseAddr,
                         MachinePointerInfo::getFixedStack(MF, FalseFI),
                         Align(8));

    // i32 select: the legalizer expands it into the branchless mask sequence
    // from chapter 4 (SELECT_CC), which is why SELECT must stay Custom.
    SDValue Addr =
        DAG.getNode(ISD::SELECT, DL, PtrVT, Cond, TrueAddr, FalseAddr);
    return DAG.getLoad(MVT::f64, DL, Chain, Addr, MachinePointerInfo(),
                       Align(8));
  }

  SDValue TrueInt = DAG.getBitcast(MVT::i32, TrueV);
  SDValue FalseInt = DAG.getBitcast(MVT::i32, FalseV);
  SDValue Sel = DAG.getNode(ISD::SELECT, DL, MVT::i32, Cond, TrueInt, FalseInt);
  return DAG.getBitcast(MVT::f32, Sel);
}

// Chapter 41: f64 immediate.  Sequenced before the first use through its own
// chain; the slot is private, so it needs no ordering against other memory
// operations.  A constant pool would need PC-relative addressing that RVMin
// does not have, and i64 is not a legal type to bitcast through.
SDValue RVMinTargetLowering::lowerConstantFP(SDValue Op,
                                             SelectionDAG &DAG) const {
  assert(Op.getValueType() == MVT::f64 && "only double immediates are custom");
  APInt Bits =
      cast<ConstantFPSDNode>(Op)->getValueAPF().bitcastToAPInt();

  MachineFunction &MF = DAG.getMachineFunction();
  int FI = MF.getFrameInfo().CreateStackObject(8, Align(8),
                                               /*isSpillSlot=*/false);
  SDLoc DL(Op);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue FIN = DAG.getFrameIndex(FI, PtrVT);
  MachinePointerInfo MPI = MachinePointerInfo::getFixedStack(MF, FI);

  SDValue Chain = DAG.getEntryNode();
  Chain = DAG.getStore(Chain, DL,
                       DAG.getConstant(Bits.trunc(32).getZExtValue(), DL,
                                       MVT::i32),
                       FIN, MPI, Align(4));
  Chain = DAG.getStore(Chain, DL,
                       DAG.getConstant(Bits.lshr(32).getZExtValue(), DL,
                                       MVT::i32),
                       DAG.getMemBasePlusOffset(FIN, TypeSize::getFixed(4), DL),
                       MPI.getWithOffset(4), Align(4));

  return DAG.getLoad(MVT::f64, DL, Chain, FIN, MPI, Align(8));
}

// Chapter 51: split an f64 into two i32 halves through a private 8-byte slot.
// The slot never escapes, so the chain starts at the entry node (see
// lowerVectorThroughStack for why using the current root can create a cycle).
std::pair<SDValue, SDValue>
RVMinTargetLowering::splitF64ToI32Pair(SDValue V, const SDLoc &DL,
                                      SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  int FI = MFI.CreateStackObject(8, Align(8), /*isSpillSlot=*/false);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue FIN = DAG.getFrameIndex(FI, PtrVT);
  MachinePointerInfo MPI = MachinePointerInfo::getFixedStack(MF, FI);

  SDValue Chain = DAG.getStore(DAG.getEntryNode(), DL, V, FIN, MPI, Align(8));
  SDValue Lo = DAG.getLoad(MVT::i32, DL, Chain, FIN, MPI, Align(4));
  SDValue HiAddr =
      DAG.getMemBasePlusOffset(FIN, TypeSize::getFixed(4), DL);
  SDValue Hi = DAG.getLoad(MVT::i32, DL, Chain, HiAddr, MPI.getWithOffset(4),
                           Align(4));
  return {Lo, Hi};
}

// Chapter 51: the reverse direction.  Store the two halves and reload the
// double; this is the same trick lowerConstantFP uses for f64 constants.
SDValue RVMinTargetLowering::joinI32PairToF64(SDValue Lo, SDValue Hi,
                                              const SDLoc &DL,
                                              SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  int FI = MFI.CreateStackObject(8, Align(8), /*isSpillSlot=*/false);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue FIN = DAG.getFrameIndex(FI, PtrVT);
  MachinePointerInfo MPI = MachinePointerInfo::getFixedStack(MF, FI);

  SDValue Chain = DAG.getEntryNode();
  Chain = DAG.getStore(Chain, DL, Lo, FIN, MPI, Align(4));
  SDValue HiAddr =
      DAG.getMemBasePlusOffset(FIN, TypeSize::getFixed(4), DL);
  Chain = DAG.getStore(Chain, DL, Hi, HiAddr, MPI.getWithOffset(4), Align(4));
  return DAG.getLoad(MVT::f64, DL, Chain, FIN, MPI, Align(8));
}

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
  checkABICompatibility(MF);

  // Chapter 55: only the LMUL=1 integer vector type has an ABI in this
  // chapter.  LMUL=2 needs an even-aligned register pair and is deferred.
  const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();
  if (STI.hasStdExtV()) {
    for (const ISD::InputArg &Arg : Ins) {
      if (!Arg.VT.isVector())
        continue;
      if (isLMUL2VectorABIType(Arg.VT))
        report_fatal_error(
            "RVMin: LMUL=2 vector arguments are not supported yet",
            /*gen_crash_diag=*/false);
      if (!isSupportedVectorABIType(Arg.VT))
        report_fatal_error("RVMin: vector arguments are not supported yet",
                           /*gen_crash_diag=*/false);
    }
  }

  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_RVMin);

  MachineFrameInfo &MFI = MF.getFrameInfo();

  // Chapter 51: one value may occupy several locations (an f64 in the
  // soft-float ABI is two i32 locations), so group by the value number
  // instead of assuming ArgLocs[I] belongs to Ins[I].
  SmallVector<SmallVector<SDValue, 2>, 8> Pieces(Ins.size());
  for (CCValAssign &VA : ArgLocs) {
    unsigned ValNo = VA.getValNo();
    assert(ValNo < Ins.size() && "argument location without a value");
    MVT LocVT = VA.getLocVT();

    if (VA.isRegLoc()) {
      // Arguments 1..8 arrive in a0-a7, or in fa0-fa7 under a hard-float ABI.
      // LocVT is the register-sized type from the CC table, so the register
      // class follows from it: i32 -> GPR, f32 -> FPR32.
      Register VReg = RegInfo.createVirtualRegister(getRegClassFor(LocVT));
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      Pieces[ValNo].push_back(DAG.getCopyFromReg(Chain, DL, VReg, LocVT));
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
    Pieces[ValNo].push_back(
        DAG.getLoad(LocVT, DL, Chain, FIN,
                    MachinePointerInfo::getFixedStack(MF, FI)));
  }

  for (unsigned I = 0, E = Ins.size(); I != E; ++I) {
    MVT ArgVT = Ins[I].VT;
    auto &P = Pieces[I];
    if (P.size() == 2 && ArgVT == MVT::f64) {
      // Soft-float f64: two i32 halves arrive in a register pair.
      InVals.push_back(joinI32PairToF64(P[0], P[1], DL, DAG));
      continue;
    }
    if (P.size() != 1)
      report_fatal_error("RVMin: unsupported multi-location argument",
                         /*gen_crash_diag=*/false);

    SDValue V = P[0];
    if (ArgVT == MVT::f32 && V.getValueType() != MVT::f32)
      V = DAG.getBitcast(MVT::f32, V);
    InVals.push_back(V);
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

  // Chapter 55: the same LMUL=1 contract applies at a call site.  When V is
  // disabled the type legalizer has already scalarized the value.
  const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();
  if (STI.hasStdExtV()) {
    for (const ISD::OutputArg &Arg : Outs) {
      if (!Arg.VT.isVector())
        continue;
      if (isLMUL2VectorABIType(Arg.VT))
        report_fatal_error(
            "RVMin: LMUL=2 vector arguments are not supported yet",
            /*gen_crash_diag=*/false);
      if (!isSupportedVectorABIType(Arg.VT))
        report_fatal_error("RVMin: vector arguments are not supported yet",
                           /*gen_crash_diag=*/false);
    }
    for (const ISD::InputArg &Arg : Ins) {
      if (!Arg.VT.isVector())
        continue;
      if (isLMUL2VectorABIType(Arg.VT))
        report_fatal_error("RVMin: LMUL=2 vector returns are not supported yet",
                           /*gen_crash_diag=*/false);
      if (!isSupportedVectorABIType(Arg.VT))
        report_fatal_error("RVMin: vector returns are not supported yet",
                           /*gen_crash_diag=*/false);
    }
  }

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

  // Chapter 51: an f64 in the soft-float ABI is one value at two locations.
  // Split it once (through a private stack slot), then hand each location its
  // half.  Hard-float f64 stays a single f64 location.
  SmallVector<std::pair<SDValue, SDValue>, 4> SplitF64(OutVals.size());
  SmallVector<unsigned, 4> NextHalf(OutVals.size(), 0);

  for (CCValAssign &VA : ArgLocs) {
    unsigned ValNo = VA.getValNo();
    assert(ValNo < OutVals.size() && "argument location without a value");
    SDValue ArgValue = OutVals[ValNo];

    if (VA.getLocInfo() == CCValAssign::BCvt) {
      if (ArgValue.getValueType() == MVT::f32) {
        ArgValue = DAG.getBitcast(MVT::i32, ArgValue);
      } else {
        if (!SplitF64[ValNo].first.getNode())
          SplitF64[ValNo] = splitF64ToI32Pair(ArgValue, DL, DAG);
        unsigned Half = NextHalf[ValNo]++;
        ArgValue = Half == 0 ? SplitF64[ValNo].first : SplitF64[ValNo].second;
      }
    }

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
    MaybeAlign Alignment;
    if (ArgValue.getValueType() == MVT::v4i32)
      Alignment = Align(16);
    MemOpChains.push_back(DAG.getStore(
        Chain, DL, ArgValue, Address,
        MachinePointerInfo::getStack(MF, VA.getLocMemOffset()), Alignment));
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

  // Copy the results out of a0/a1.  Chapter 51: a soft-float f64 result is
  // two i32 locations and is joined through a private stack slot.
  SmallVector<CCValAssign, 4> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(Ins, RetCC_RVMin);

  SmallVector<SmallVector<SDValue, 2>, 4> Pieces(Ins.size());
  for (CCValAssign &VA : RVLocs) {
    assert(VA.isRegLoc() && "only register results are supported");
    SDValue Copy =
        DAG.getCopyFromReg(Chain, DL, VA.getLocReg(), VA.getLocVT(), Glue);
    Chain = Copy.getValue(1);
    Glue = Copy.getValue(2);
    Pieces[VA.getValNo()].push_back(Copy.getValue(0));
  }

  for (unsigned I = 0, E = Ins.size(); I != E; ++I) {
    MVT RetVT = Ins[I].VT;
    auto &P = Pieces[I];
    if (P.size() == 2 && RetVT == MVT::f64) {
      InVals.push_back(joinI32PairToF64(P[0], P[1], DL, DAG));
      continue;
    }
    if (P.size() != 1)
      report_fatal_error("RVMin: unsupported multi-location return value",
                         /*gen_crash_diag=*/false);

    SDValue V = P[0];
    if (RetVT == MVT::f32 && V.getValueType() != MVT::f32)
      V = DAG.getBitcast(MVT::f32, V);
    InVals.push_back(V);
  }

  return Chain;
}

bool RVMinTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  // Chapter 55: a v4i32 return goes in v8.  Without V the value is demoted
  // through the generic sret path, so every rv32i/+m/+c gate run still works.
  const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();
  const bool HasV = STI.hasStdExtV();
  if (RetTy && RetTy->isVectorTy()) {
    if (!HasV)
      return false;
    if (isLMUL2VectorABIType(RetTy))
      report_fatal_error("RVMin: LMUL=2 vector returns are not supported yet",
                         /*gen_crash_diag=*/false);
    if (!isSupportedVectorABIType(RetTy))
      report_fatal_error("RVMin: vector returns are not supported yet",
                         /*gen_crash_diag=*/false);
  }
  for (const ISD::OutputArg &Arg : Outs) {
    if (!Arg.VT.isVector())
      continue;
    if (!HasV)
      return false;
    if (isLMUL2VectorABIType(Arg.VT))
      report_fatal_error("RVMin: LMUL=2 vector returns are not supported yet",
                         /*gen_crash_diag=*/false);
    if (!isSupportedVectorABIType(Arg.VT))
      report_fatal_error("RVMin: vector returns are not supported yet",
                         /*gen_crash_diag=*/false);
  }

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

  // Defense in depth: CanLowerReturn normally rejects unsupported vectors
  // first.  With V disabled a vector return never reaches this function; the
  // generic sret demotion path handles it.
  const RVMinSubtarget &STI = MF.getSubtarget<RVMinSubtarget>();
  if (STI.hasStdExtV()) {
    for (const ISD::OutputArg &Arg : Outs) {
      if (!Arg.VT.isVector())
        continue;
      if (isLMUL2VectorABIType(Arg.VT))
        report_fatal_error("RVMin: LMUL=2 vector returns are not supported yet",
                           /*gen_crash_diag=*/false);
      if (!isSupportedVectorABIType(Arg.VT))
        report_fatal_error("RVMin: vector returns are not supported yet",
                           /*gen_crash_diag=*/false);
    }
  }

  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  if (!CCInfo.CheckReturn(Outs, RetCC_RVMin))
    return DAG.getNode(ISD::UNDEF, DL, MVT::Other);

  // Move the return values into their ABI registers and terminate with
  // RET_FLAG, which TableGen maps to PseudoRET / `jalr zero, 0(ra)`.
  // Chapter 51: a soft-float f64 return is two i32 locations, so split it
  // once and copy each half to a0/a1.
  SmallVector<std::pair<SDValue, SDValue>, 4> SplitF64(OutVals.size());
  SmallVector<unsigned, 4> NextHalf(OutVals.size(), 0);

  SmallVector<SDValue, 4> RetOps(1, Chain);
  SDValue Glue;
  for (CCValAssign &VA : RVLocs) {
    assert(VA.isRegLoc() && "can only return in registers");
    unsigned ValNo = VA.getValNo();
    SDValue Val = OutVals[ValNo];

    if (VA.getLocInfo() == CCValAssign::BCvt) {
      if (Val.getValueType() == MVT::f32) {
        Val = DAG.getBitcast(MVT::i32, Val);
      } else {
        if (!SplitF64[ValNo].first.getNode())
          SplitF64[ValNo] = splitF64ToI32Pair(Val, DL, DAG);
        unsigned Half = NextHalf[ValNo]++;
        Val = Half == 0 ? SplitF64[ValNo].first : SplitF64[ValNo].second;
      }
    }

    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), Val, Glue);
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
