//===- X86LegalizerInfo.cpp --------------------------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the targeting of the Machinelegalizer class for X86.
/// The rule chains are generated from X86LegalizerInfo.td; this file keeps
/// the custom legalization and the shared type/feature setup.
//===----------------------------------------------------------------------===//

#include "X86LegalizerInfo.h"
#include "X86LegalizerTable.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "llvm/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/MIPatternMatch.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace MIPatternMatch;
using namespace TargetOpcode;
using namespace LegalizeActions;
using namespace LegalityPredicates;

static cl::opt<bool> X86GISelTable(
    "x86-gisel-table", cl::Hidden, cl::init(true),
    cl::desc(
        "Answer X86 legalizer queries from a table instead of the rule chains "
        "(-x86-gisel-table=false goes back to the chains)"));

static cl::opt<bool> X86GISelTableCheck(
    "x86-gisel-table-check", cl::Hidden, cl::init(false),
    cl::desc("Check the X86 legalizer table against the rule chains"));

X86LegalizerInfo::X86LegalizerInfo(const X86Subtarget &STI,
                                   const X86TargetMachine &TM)
    : Subtarget(STI) {

  // The subtarget-feature/LLT vocabulary and the rule chains are both
  // generated from GISel/X86LegalizerInfo.td.  The generated file is a
  // statement sequence, so it stays inside the constructor and the member
  // Subtarget stays in scope.
#include "X86LegalizerInfo.inc"

  verify(*STI.getInstrInfo());

  // The table's key pins the two subtarget-dependent LLT constants (the pointer
  // width and the widest scalar) to their 64-bit values, which is what makes a
  // class list that does not vary with the subtarget possible.  That pin is
  // only sound where the pin holds, so a 32-bit subtarget keeps the chains.
  // LLT::pointer(0) is 64 bits under LP64 and 32 under ILP32; the class list
  // pins it to 64, so the self-check's representatives follow the same
  // expression and the points it cannot key are reported as Outside.
  const unsigned PointerBits = STI.isTarget64BitLP64() ? 64 : 32;

  if (X86GISelTable && STI.is64Bit())
    setTable(std::make_unique<X86LegalizerTable>(*this, PointerBits));

  if (X86GISelTableCheck) {
    X86LegalizerTable Table(*this, PointerBits);
    Table.selfCheck(errs());
  }
}

bool X86LegalizerInfo::legalizeCustom(LegalizerHelper &Helper, MachineInstr &MI,
                                      LostDebugLocObserver &LocObserver) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  MachineRegisterInfo &MRI = *MIRBuilder.getMRI();
  switch (MI.getOpcode()) {
  default:
    // No idea what to do.
    return false;
  case TargetOpcode::G_BUILD_VECTOR:
    return legalizeBuildVector(MI, MRI, Helper);
  case TargetOpcode::G_FPTOUI:
    return legalizeFPTOUI(MI, MRI, Helper);
  case TargetOpcode::G_UITOFP:
    return legalizeUITOFP(MI, MRI, Helper);
  case TargetOpcode::G_STORE:
    return legalizeNarrowingStore(MI, MRI, Helper);
  case TargetOpcode::G_SITOFP:
    return legalizeSITOFP(MI, MRI, Helper);
  case TargetOpcode::G_FPTOSI:
    return legalizeFPTOSI(MI, MRI, Helper);
  case TargetOpcode::G_GET_ROUNDING:
    return legalizeGETROUNDING(MI, MRI, Helper);
  case TargetOpcode::G_SET_ROUNDING:
    return legalizeSETROUNDING(MI, MRI, Helper);
  case TargetOpcode::G_GLOBAL_VALUE:
    return legalizeGLOBAL_VALUE(MI, MRI, Helper);
  }
  llvm_unreachable("expected switch to return");
}

bool X86LegalizerInfo::legalizeSITOFP(MachineInstr &MI,
                                      MachineRegisterInfo &MRI,
                                      LegalizerHelper &Helper) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  MachineFunction &MF = *MI.getMF();
  auto [Dst, DstTy, Src, SrcTy] = MI.getFirst2RegLLTs();

  assert((SrcTy.getSizeInBits() == 16 || SrcTy.getSizeInBits() == 32 ||
          SrcTy.getSizeInBits() == 64) &&
         "Unexpected source type for SITOFP in X87 mode.");

  TypeSize MemSize = SrcTy.getSizeInBytes();
  MachinePointerInfo PtrInfo;
  Align Alignmt = Helper.getStackTemporaryAlignment(SrcTy);
  auto SlotPointer = Helper.createStackTemporary(MemSize, Alignmt, PtrInfo);
  MachineMemOperand *StoreMMO = MF.getMachineMemOperand(
      PtrInfo, MachineMemOperand::MOStore, MemSize, Align(MemSize));

  // Store the integer value on the FPU stack.
  MIRBuilder.buildStore(Src, SlotPointer, *StoreMMO);

  MachineMemOperand *LoadMMO = MF.getMachineMemOperand(
      PtrInfo, MachineMemOperand::MOLoad, MemSize, Align(MemSize));
  MIRBuilder.buildInstr(X86::G_FILD)
      .addDef(Dst)
      .addUse(SlotPointer.getReg(0))
      .addMemOperand(LoadMMO);

  MI.eraseFromParent();
  return true;
}

bool X86LegalizerInfo::legalizeFPTOSI(MachineInstr &MI,
                                      MachineRegisterInfo &MRI,
                                      LegalizerHelper &Helper) const {
  MachineFunction &MF = *MI.getMF();
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  auto [Dst, DstTy, Src, SrcTy] = MI.getFirst2RegLLTs();

  TypeSize MemSize = DstTy.getSizeInBytes();
  MachinePointerInfo PtrInfo;
  Align Alignmt = Helper.getStackTemporaryAlignment(DstTy);
  auto SlotPointer = Helper.createStackTemporary(MemSize, Alignmt, PtrInfo);
  MachineMemOperand *StoreMMO = MF.getMachineMemOperand(
      PtrInfo, MachineMemOperand::MOStore, MemSize, Align(MemSize));

  MIRBuilder.buildInstr(X86::G_FIST)
      .addUse(Src)
      .addUse(SlotPointer.getReg(0))
      .addMemOperand(StoreMMO);

  MIRBuilder.buildLoad(Dst, SlotPointer, PtrInfo, Align(MemSize));
  MI.eraseFromParent();
  return true;
}

bool X86LegalizerInfo::legalizeBuildVector(MachineInstr &MI,
                                           MachineRegisterInfo &MRI,
                                           LegalizerHelper &Helper) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  const auto &BuildVector = cast<GBuildVector>(MI);
  Register Dst = BuildVector.getReg(0);
  LLT DstTy = MRI.getType(Dst);
  MachineFunction &MF = MIRBuilder.getMF();
  LLVMContext &Ctx = MF.getFunction().getContext();
  uint64_t DstTySize = DstTy.getScalarSizeInBits();

  SmallVector<Constant *, 4> CstIdxs;
  for (unsigned i = 0; i < BuildVector.getNumSources(); ++i) {
    Register Source = BuildVector.getSourceReg(i);

    auto ValueAndReg = getIConstantVRegValWithLookThrough(Source, MRI);
    if (ValueAndReg) {
      CstIdxs.emplace_back(ConstantInt::get(Ctx, ValueAndReg->Value));
      continue;
    }

    auto FPValueAndReg = getFConstantVRegValWithLookThrough(Source, MRI);
    if (FPValueAndReg) {
      CstIdxs.emplace_back(ConstantFP::get(Ctx, FPValueAndReg->Value));
      continue;
    }

    if (getOpcodeDef<GImplicitDef>(Source, MRI)) {
      CstIdxs.emplace_back(UndefValue::get(Type::getIntNTy(Ctx, DstTySize)));
      continue;
    }
    return false;
  }

  Constant *ConstVal = ConstantVector::get(CstIdxs);

  const DataLayout &DL = MIRBuilder.getDataLayout();
  unsigned AddrSpace = DL.getDefaultGlobalsAddressSpace();
  Align Alignment(DL.getABITypeAlign(ConstVal->getType()));
  auto Addr = MIRBuilder.buildConstantPool(
      LLT::pointer(AddrSpace, DL.getPointerSizeInBits(AddrSpace)),
      MF.getConstantPool()->getConstantPoolIndex(ConstVal, Alignment));
  MachineMemOperand *MMO =
      MF.getMachineMemOperand(MachinePointerInfo::getConstantPool(MF),
                              MachineMemOperand::MOLoad, DstTy, Alignment);

  MIRBuilder.buildLoad(Dst, Addr, *MMO);
  MI.eraseFromParent();
  return true;
}

bool X86LegalizerInfo::legalizeFPTOUI(MachineInstr &MI,
                                      MachineRegisterInfo &MRI,
                                      LegalizerHelper &Helper) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  auto [Dst, DstTy, Src, SrcTy] = MI.getFirst2RegLLTs();
  unsigned DstSizeInBits = DstTy.getScalarSizeInBits();
  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);

  // Simply reuse FPTOSI when it is possible to widen the type
  if (DstSizeInBits <= 32) {
    auto Casted = MIRBuilder.buildFPTOSI(DstTy == s32 ? s64 : s32, Src);
    MIRBuilder.buildTrunc(Dst, Casted);
    MI.eraseFromParent();
    return true;
  }

  return false;
}

bool X86LegalizerInfo::legalizeUITOFP(MachineInstr &MI,
                                      MachineRegisterInfo &MRI,
                                      LegalizerHelper &Helper) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  auto [Dst, DstTy, Src, SrcTy] = MI.getFirst2RegLLTs();
  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);

  // Simply reuse SITOFP when it is possible to widen the type
  if (SrcTy.getSizeInBits() <= 32) {
    auto Ext = MIRBuilder.buildZExt(SrcTy == s32 ? s64 : s32, Src);
    MIRBuilder.buildSITOFP(Dst, Ext);
    MI.eraseFromParent();
    return true;
  }

  return false;
}

bool X86LegalizerInfo::legalizeNarrowingStore(MachineInstr &MI,
                                              MachineRegisterInfo &MRI,
                                              LegalizerHelper &Helper) const {
  auto &Store = cast<GStore>(MI);
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  MachineMemOperand &MMO = **Store.memoperands_begin();
  MachineFunction &MF = MIRBuilder.getMF();
  LLT ValTy = MRI.getType(Store.getValueReg());
  auto *NewMMO = MF.getMachineMemOperand(&MMO, MMO.getPointerInfo(), ValTy);

  Helper.Observer.changingInstr(Store);
  Store.setMemRefs(MF, {NewMMO});
  Helper.Observer.changedInstr(Store);
  return true;
}

bool X86LegalizerInfo::legalizeGETROUNDING(MachineInstr &MI,
                                           MachineRegisterInfo &MRI,
                                           LegalizerHelper &Helper) const {
  /*
   The rounding mode is in bits 11:10 of FPSR, and has the following
   settings:
     00 Round to nearest
     01 Round to -inf
     10 Round to +inf
     11 Round to 0

  GET_ROUNDING, on the other hand, expects the following:
    -1 Undefined
     0 Round to 0
     1 Round to nearest
     2 Round to +inf
     3 Round to -inf

  To perform the conversion, we use a packed lookup table of the four 2-bit
  values that we can index by FPSP[11:10]
    0x2d --> (0b00,10,11,01) --> (0,2,3,1) >> FPSR[11:10]

    (0x2d >> ((FPSR >> 9) & 6)) & 3
  */

  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  MachineFunction &MF = MIRBuilder.getMF();
  Register Dst = MI.getOperand(0).getReg();
  LLT DstTy = MRI.getType(Dst);
  const LLT s8 = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);

  // Save FP Control Word to stack slot
  int MemSize = 2;
  Align Alignment = Align(2);
  MachinePointerInfo PtrInfo;
  auto StackTemp = Helper.createStackTemporary(TypeSize::getFixed(MemSize),
                                               Alignment, PtrInfo);
  Register StackPtr = StackTemp.getReg(0);

  auto StoreMMO = MF.getMachineMemOperand(PtrInfo, MachineMemOperand::MOStore,
                                          MemSize, Alignment);

  // Store FP Control Word to stack slot using G_FNSTCW16
  MIRBuilder.buildInstr(X86::G_FNSTCW16)
      .addUse(StackPtr)
      .addMemOperand(StoreMMO);

  // Load FP Control Word from stack slot
  auto LoadMMO = MF.getMachineMemOperand(PtrInfo, MachineMemOperand::MOLoad,
                                         MemSize, Alignment);

  auto CWD32 =
      MIRBuilder.buildZExt(s32, MIRBuilder.buildLoad(s16, StackPtr, *LoadMMO));
  auto Shifted8 = MIRBuilder.buildTrunc(
      s8, MIRBuilder.buildLShr(s32, CWD32, MIRBuilder.buildConstant(s8, 9)));
  auto Masked32 = MIRBuilder.buildZExt(
      s32, MIRBuilder.buildAnd(s8, Shifted8, MIRBuilder.buildConstant(s8, 6)));

  // LUT is a packed lookup table (0x2d) used to map the 2-bit x87 FPU rounding
  // mode (from bits 11:10 of the control word) to the values expected by
  // GET_ROUNDING. The mapping is performed by shifting LUT right by the
  // extracted rounding mode and masking the result with 3 to obtain the final
  auto LUT = MIRBuilder.buildConstant(s32, 0x2d);
  auto LUTShifted = MIRBuilder.buildLShr(s32, LUT, Masked32);
  auto RetVal =
      MIRBuilder.buildAnd(s32, LUTShifted, MIRBuilder.buildConstant(s32, 3));
  auto RetValTrunc = MIRBuilder.buildZExtOrTrunc(DstTy, RetVal);

  MIRBuilder.buildCopy(Dst, RetValTrunc);

  MI.eraseFromParent();
  return true;
}

bool X86LegalizerInfo::legalizeSETROUNDING(MachineInstr &MI,
                                           MachineRegisterInfo &MRI,
                                           LegalizerHelper &Helper) const {
  MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
  MachineFunction &MF = MIRBuilder.getMF();
  Register Src = MI.getOperand(0).getReg();
  const LLT s8 = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);

  // Allocate stack slot for control word and MXCSR (4 bytes).
  int MemSize = 4;
  Align Alignment = Align(4);
  MachinePointerInfo PtrInfo;
  auto StackTemp = Helper.createStackTemporary(TypeSize::getFixed(MemSize),
                                               Alignment, PtrInfo);
  Register StackPtr = StackTemp.getReg(0);

  auto StoreMMO =
      MF.getMachineMemOperand(PtrInfo, MachineMemOperand::MOStore, 2, Align(2));
  MIRBuilder.buildInstr(X86::G_FNSTCW16)
      .addUse(StackPtr)
      .addMemOperand(StoreMMO);

  auto LoadMMO =
      MF.getMachineMemOperand(PtrInfo, MachineMemOperand::MOLoad, 2, Align(2));
  auto CWD16 = MIRBuilder.buildLoad(s16, StackPtr, *LoadMMO);

  // Clear RM field (bits 11:10)
  auto ClearedCWD =
      MIRBuilder.buildAnd(s16, CWD16, MIRBuilder.buildConstant(s16, 0xf3ff));

  // Check if Src is a constant
  Register RMBits;
  Register MXCSRRMBits;

  APInt SrcCst;
  if (mi_match(Src, MRI, m_ICst(SrcCst))) {
    uint64_t RM = SrcCst.getZExtValue();
    int FieldVal = X86::getRoundingModeX86(RM);

    if (FieldVal == X86::rmInvalid) {
      FieldVal = X86::rmToNearest;
      LLVMContext &C = MF.getFunction().getContext();
      C.diagnose(DiagnosticInfoUnsupported(
          MF.getFunction(), "rounding mode is not supported by X86 hardware",
          DiagnosticLocation(MI.getDebugLoc()), DS_Error));
      return false;
    }

    FieldVal = FieldVal << 3;
    RMBits = MIRBuilder.buildConstant(s16, FieldVal).getReg(0);
    MXCSRRMBits = MIRBuilder.buildConstant(s32, FieldVal).getReg(0);
  } else {
    // Convert Src (rounding mode) to bits for control word
    // (0xc9 << (2 * Src + 4)) & 0xc00
    auto Src32 = MIRBuilder.buildZExtOrTrunc(s32, Src);
    auto ShiftAmt = MIRBuilder.buildAdd(
        s32, MIRBuilder.buildShl(s32, Src32, MIRBuilder.buildConstant(s32, 1)),
        MIRBuilder.buildConstant(s32, 4));
    auto ShiftAmt8 = MIRBuilder.buildTrunc(s8, ShiftAmt);
    auto Shifted = MIRBuilder.buildShl(s16, MIRBuilder.buildConstant(s16, 0xc9),
                                       ShiftAmt8);
    RMBits =
        MIRBuilder.buildAnd(s16, Shifted, MIRBuilder.buildConstant(s16, 0xc00))
            .getReg(0);

    // For non-constant case, we still need to compute MXCSR bits dynamically
    auto RMBits32 = MIRBuilder.buildZExt(s32, RMBits);
    MXCSRRMBits =
        MIRBuilder.buildShl(s32, RMBits32, MIRBuilder.buildConstant(s32, 3))
            .getReg(0);
  }
  // Update rounding mode bits
  auto NewCWD =
      MIRBuilder.buildOr(s16, ClearedCWD, RMBits, MachineInstr::Disjoint);

  // Store new FP Control Word to stack
  auto StoreNewMMO =
      MF.getMachineMemOperand(PtrInfo, MachineMemOperand::MOStore, 2, Align(2));
  MIRBuilder.buildStore(NewCWD, StackPtr, *StoreNewMMO);

  // Load FP control word from the slot using G_FLDCW16
  auto LoadNewMMO =
      MF.getMachineMemOperand(PtrInfo, MachineMemOperand::MOLoad, 2, Align(2));
  MIRBuilder.buildInstr(X86::G_FLDCW16)
      .addUse(StackPtr)
      .addMemOperand(LoadNewMMO);

  if (Subtarget.hasSSE1()) {
    // Store MXCSR to stack (use STMXCSR)
    auto StoreMXCSRMMO = MF.getMachineMemOperand(
        PtrInfo, MachineMemOperand::MOStore, 4, Align(4));
    MIRBuilder.buildInstr(TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS)
        .addIntrinsicID(Intrinsic::x86_sse_stmxcsr)
        .addUse(StackPtr)
        .addMemOperand(StoreMXCSRMMO);

    // Load MXCSR from stack
    auto LoadMXCSRMMO = MF.getMachineMemOperand(
        PtrInfo, MachineMemOperand::MOLoad, 4, Align(4));
    auto MXCSR = MIRBuilder.buildLoad(s32, StackPtr, *LoadMXCSRMMO);

    // Clear RM field (bits 14:13)
    auto ClearedMXCSR = MIRBuilder.buildAnd(
        s32, MXCSR, MIRBuilder.buildConstant(s32, 0xffff9fff));

    // Update rounding mode bits
    auto NewMXCSR = MIRBuilder.buildOr(s32, ClearedMXCSR, MXCSRRMBits);

    // Store new MXCSR to stack
    auto StoreNewMXCSRMMO = MF.getMachineMemOperand(
        PtrInfo, MachineMemOperand::MOStore, 4, Align(4));
    MIRBuilder.buildStore(NewMXCSR, StackPtr, *StoreNewMXCSRMMO);

    // Load MXCSR from stack (use LDMXCSR)
    auto LoadNewMXCSRMMO = MF.getMachineMemOperand(
        PtrInfo, MachineMemOperand::MOLoad, 4, Align(4));
    MIRBuilder.buildInstr(TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS)
        .addIntrinsicID(Intrinsic::x86_sse_ldmxcsr)
        .addUse(StackPtr)
        .addMemOperand(LoadNewMXCSRMMO);
  }

  MI.eraseFromParent();
  return true;
}

bool X86LegalizerInfo::legalizeGLOBAL_VALUE(MachineInstr &MI,
                                            MachineRegisterInfo &MRI,
                                            LegalizerHelper &Helper) const {
  const GlobalValue *GV = MI.getOperand(1).getGlobal();
  Register Dst = MI.getOperand(0).getReg();
  LLT DstTy = MRI.getType(Dst);
  unsigned GVOpFlags = Subtarget.classifyGlobalReference(GV);

  // For stub references (GOT/PLT), we need G_WRAPPER_RIP + load
  if (isGlobalStubReference(GVOpFlags)) {
    MachineIRBuilder &MIRBuilder = Helper.MIRBuilder;
    MachineFunction &MF = MIRBuilder.getMF();

    Register StubAddr = MRI.createGenericVirtualRegister(DstTy);
    MIRBuilder.buildInstr(X86::G_WRAPPER_RIP)
        .addDef(StubAddr)
        .addGlobalAddress(GV);

    MachineMemOperand *MMO = MF.getMachineMemOperand(
        MachinePointerInfo::getGOT(MF), MachineMemOperand::MOLoad, DstTy,
        Align(DstTy.getSizeInBytes()));
    MIRBuilder.buildLoad(Dst, StubAddr, *MMO);
    MI.eraseFromParent();
  }
  return true;
}

bool X86LegalizerInfo::legalizeIntrinsic(LegalizerHelper &Helper,
                                         MachineInstr &MI) const {
  return true;
}
