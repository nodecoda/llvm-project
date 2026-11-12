//===-- RVTrimCallingConv.cpp - RISC-V Custom CC Routines ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the custom routines for the RISC-V Calling Convention.
//
//===----------------------------------------------------------------------===//

#include "RVTrimCallingConv.h"
#include "RVTrimMachineFunctionInfo.h"
#include "RVTrimSubtarget.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/MC/MCRegister.h"

using namespace llvm;

// This does not have the regular `CCAssignFn` signature, it has an extra
// `bool IsRet` parameter.
static bool CC_RVTrim_Impl(unsigned ValNo, MVT ValVT, MVT LocVT,
                          CCValAssign::LocInfo LocInfo,
                          ISD::ArgFlagsTy ArgFlags, Type *OrigTy,
                          CCState &State, bool IsRet);

/// Used for assigning arguments with CallingConvention::GHC
static CCAssignFn CC_RVTrim_GHC;

/// Used for assigning arguments with CallingConvention::Fast
static CCAssignFn CC_RVTrim_FastCC;

bool llvm::CC_RVTrim(unsigned ValNo, MVT ValVT, MVT LocVT,
                    CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags,
                    Type *OrigTy, CCState &State) {
  if (State.getCallingConv() == CallingConv::GHC)
    return CC_RVTrim_GHC(ValNo, ValVT, LocVT, LocInfo, ArgFlags, OrigTy, State);

  if (State.getCallingConv() == CallingConv::Fast)
    return CC_RVTrim_FastCC(ValNo, ValVT, LocVT, LocInfo, ArgFlags, OrigTy,
                           State);

  // For all other cases, use the standard calling convention
  return CC_RVTrim_Impl(ValNo, ValVT, LocVT, LocInfo, ArgFlags, OrigTy, State,
                       /*IsRet=*/false);
}

bool llvm::RetCC_RVTrim(unsigned ValNo, MVT ValVT, MVT LocVT,
                       CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags,
                       Type *OrigTy, CCState &State) {
  // Always use the standard calling convention.
  return CC_RVTrim_Impl(ValNo, ValVT, LocVT, LocInfo, ArgFlags, OrigTy, State,
                       /*IsRet=*/true);
}

// Calling Convention Implementation.
// The expectations for frontend ABI lowering vary from target to target.
// Ideally, an LLVM frontend would be able to avoid worrying about many ABI
// details, but this is a longer term goal. For now, we simply try to keep the
// role of the frontend as simple and well-defined as possible. The rules can
// be summarised as:
// * Never split up large scalar arguments. We handle them here.
// * If a hardfloat calling convention is being used, and the struct may be
// passed in a pair of registers (fp+fp, int+fp), and both registers are
// available, then pass as two separate arguments. If either the GPRs or FPRs
// are exhausted, then pass according to the rule below.
// * If a struct could never be passed in registers or directly in a stack
// slot (as it is larger than 2*XLEN and the floating point rules don't
// apply), then pass it using a pointer with the byval attribute.
// * If a struct is less than 2*XLEN, then coerce to either a two-element
// word-sized array or a 2*XLEN scalar (depending on alignment).
// * The frontend can determine whether a struct is returned by reference or
// not based on its size and fields. If it will be returned by reference, the
// frontend must modify the prototype so a pointer with the sret annotation is
// passed as the first argument. This is not necessary for large scalar
// returns.
// * Struct return values and varargs should be coerced to structs containing
// register-size fields in the same situations they would be for fixed
// arguments.

static const MCPhysReg ArgFPR16s[] = {RVTrim::F10_H, RVTrim::F11_H, RVTrim::F12_H,
                                      RVTrim::F13_H, RVTrim::F14_H, RVTrim::F15_H,
                                      RVTrim::F16_H, RVTrim::F17_H};
static const MCPhysReg ArgFPR32s[] = {RVTrim::F10_F, RVTrim::F11_F, RVTrim::F12_F,
                                      RVTrim::F13_F, RVTrim::F14_F, RVTrim::F15_F,
                                      RVTrim::F16_F, RVTrim::F17_F};
static const MCPhysReg ArgFPR64s[] = {RVTrim::F10_D, RVTrim::F11_D, RVTrim::F12_D,
                                      RVTrim::F13_D, RVTrim::F14_D, RVTrim::F15_D,
                                      RVTrim::F16_D, RVTrim::F17_D};
static const MCPhysReg ArgFPR128s[] = {RVTrim::F10_Q, RVTrim::F11_Q, RVTrim::F12_Q,
                                       RVTrim::F13_Q, RVTrim::F14_Q, RVTrim::F15_Q,
                                       RVTrim::F16_Q, RVTrim::F17_Q};

// This is an interim calling convention and it may be changed in the future.
static const MCPhysReg ArgVRs[] = {
    RVTrim::V8,  RVTrim::V9,  RVTrim::V10, RVTrim::V11, RVTrim::V12, RVTrim::V13,
    RVTrim::V14, RVTrim::V15, RVTrim::V16, RVTrim::V17, RVTrim::V18, RVTrim::V19,
    RVTrim::V20, RVTrim::V21, RVTrim::V22, RVTrim::V23};
static const MCPhysReg ArgVRM2s[] = {RVTrim::V8M2,  RVTrim::V10M2, RVTrim::V12M2,
                                     RVTrim::V14M2, RVTrim::V16M2, RVTrim::V18M2,
                                     RVTrim::V20M2, RVTrim::V22M2};
static const MCPhysReg ArgVRM4s[] = {RVTrim::V8M4, RVTrim::V12M4, RVTrim::V16M4,
                                     RVTrim::V20M4};
static const MCPhysReg ArgVRM8s[] = {RVTrim::V8M8, RVTrim::V16M8};
static const MCPhysReg ArgVRN2M1s[] = {
    RVTrim::V8_V9,   RVTrim::V9_V10,  RVTrim::V10_V11, RVTrim::V11_V12,
    RVTrim::V12_V13, RVTrim::V13_V14, RVTrim::V14_V15, RVTrim::V15_V16,
    RVTrim::V16_V17, RVTrim::V17_V18, RVTrim::V18_V19, RVTrim::V19_V20,
    RVTrim::V20_V21, RVTrim::V21_V22, RVTrim::V22_V23};
static const MCPhysReg ArgVRN3M1s[] = {
    RVTrim::V8_V9_V10,   RVTrim::V9_V10_V11,  RVTrim::V10_V11_V12,
    RVTrim::V11_V12_V13, RVTrim::V12_V13_V14, RVTrim::V13_V14_V15,
    RVTrim::V14_V15_V16, RVTrim::V15_V16_V17, RVTrim::V16_V17_V18,
    RVTrim::V17_V18_V19, RVTrim::V18_V19_V20, RVTrim::V19_V20_V21,
    RVTrim::V20_V21_V22, RVTrim::V21_V22_V23};
static const MCPhysReg ArgVRN4M1s[] = {
    RVTrim::V8_V9_V10_V11,   RVTrim::V9_V10_V11_V12,  RVTrim::V10_V11_V12_V13,
    RVTrim::V11_V12_V13_V14, RVTrim::V12_V13_V14_V15, RVTrim::V13_V14_V15_V16,
    RVTrim::V14_V15_V16_V17, RVTrim::V15_V16_V17_V18, RVTrim::V16_V17_V18_V19,
    RVTrim::V17_V18_V19_V20, RVTrim::V18_V19_V20_V21, RVTrim::V19_V20_V21_V22,
    RVTrim::V20_V21_V22_V23};
static const MCPhysReg ArgVRN5M1s[] = {
    RVTrim::V8_V9_V10_V11_V12,   RVTrim::V9_V10_V11_V12_V13,
    RVTrim::V10_V11_V12_V13_V14, RVTrim::V11_V12_V13_V14_V15,
    RVTrim::V12_V13_V14_V15_V16, RVTrim::V13_V14_V15_V16_V17,
    RVTrim::V14_V15_V16_V17_V18, RVTrim::V15_V16_V17_V18_V19,
    RVTrim::V16_V17_V18_V19_V20, RVTrim::V17_V18_V19_V20_V21,
    RVTrim::V18_V19_V20_V21_V22, RVTrim::V19_V20_V21_V22_V23};
static const MCPhysReg ArgVRN6M1s[] = {
    RVTrim::V8_V9_V10_V11_V12_V13,   RVTrim::V9_V10_V11_V12_V13_V14,
    RVTrim::V10_V11_V12_V13_V14_V15, RVTrim::V11_V12_V13_V14_V15_V16,
    RVTrim::V12_V13_V14_V15_V16_V17, RVTrim::V13_V14_V15_V16_V17_V18,
    RVTrim::V14_V15_V16_V17_V18_V19, RVTrim::V15_V16_V17_V18_V19_V20,
    RVTrim::V16_V17_V18_V19_V20_V21, RVTrim::V17_V18_V19_V20_V21_V22,
    RVTrim::V18_V19_V20_V21_V22_V23};
static const MCPhysReg ArgVRN7M1s[] = {
    RVTrim::V8_V9_V10_V11_V12_V13_V14,   RVTrim::V9_V10_V11_V12_V13_V14_V15,
    RVTrim::V10_V11_V12_V13_V14_V15_V16, RVTrim::V11_V12_V13_V14_V15_V16_V17,
    RVTrim::V12_V13_V14_V15_V16_V17_V18, RVTrim::V13_V14_V15_V16_V17_V18_V19,
    RVTrim::V14_V15_V16_V17_V18_V19_V20, RVTrim::V15_V16_V17_V18_V19_V20_V21,
    RVTrim::V16_V17_V18_V19_V20_V21_V22, RVTrim::V17_V18_V19_V20_V21_V22_V23};
static const MCPhysReg ArgVRN8M1s[] = {RVTrim::V8_V9_V10_V11_V12_V13_V14_V15,
                                       RVTrim::V9_V10_V11_V12_V13_V14_V15_V16,
                                       RVTrim::V10_V11_V12_V13_V14_V15_V16_V17,
                                       RVTrim::V11_V12_V13_V14_V15_V16_V17_V18,
                                       RVTrim::V12_V13_V14_V15_V16_V17_V18_V19,
                                       RVTrim::V13_V14_V15_V16_V17_V18_V19_V20,
                                       RVTrim::V14_V15_V16_V17_V18_V19_V20_V21,
                                       RVTrim::V15_V16_V17_V18_V19_V20_V21_V22,
                                       RVTrim::V16_V17_V18_V19_V20_V21_V22_V23};
static const MCPhysReg ArgVRN2M2s[] = {RVTrim::V8M2_V10M2,  RVTrim::V10M2_V12M2,
                                       RVTrim::V12M2_V14M2, RVTrim::V14M2_V16M2,
                                       RVTrim::V16M2_V18M2, RVTrim::V18M2_V20M2,
                                       RVTrim::V20M2_V22M2};
static const MCPhysReg ArgVRN3M2s[] = {
    RVTrim::V8M2_V10M2_V12M2,  RVTrim::V10M2_V12M2_V14M2,
    RVTrim::V12M2_V14M2_V16M2, RVTrim::V14M2_V16M2_V18M2,
    RVTrim::V16M2_V18M2_V20M2, RVTrim::V18M2_V20M2_V22M2};
static const MCPhysReg ArgVRN4M2s[] = {
    RVTrim::V8M2_V10M2_V12M2_V14M2, RVTrim::V10M2_V12M2_V14M2_V16M2,
    RVTrim::V12M2_V14M2_V16M2_V18M2, RVTrim::V14M2_V16M2_V18M2_V20M2,
    RVTrim::V16M2_V18M2_V20M2_V22M2};
static const MCPhysReg ArgVRN2M4s[] = {RVTrim::V8M4_V12M4, RVTrim::V12M4_V16M4,
                                       RVTrim::V16M4_V20M4};

ArrayRef<MCPhysReg> RVTrim::getArgGPRs(const RVTrimSubtarget &STI) {
  RVTrimABI::ABI ABI = STI.getTargetABI();

  // The GPRs used for passing arguments in the ILP32* and LP64* ABIs, except
  // the ILP32E ABI.
  static const MCPhysReg ArgIGPRs[] = {RVTrim::X10, RVTrim::X11, RVTrim::X12,
                                       RVTrim::X13, RVTrim::X14, RVTrim::X15,
                                       RVTrim::X16, RVTrim::X17};
  // The GPRs used for passing arguments in the ILP32E/LP64E ABI.
  static const MCPhysReg ArgEGPRs[] = {RVTrim::X10, RVTrim::X11, RVTrim::X12,
                                       RVTrim::X13, RVTrim::X14, RVTrim::X15};

  if (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E)
    return ArrayRef(ArgEGPRs);

  return ArrayRef(ArgIGPRs);
}

ArrayRef<MCPhysReg> RVTrim::getArgFPRs(const RVTrimSubtarget &STI) {
  static const RVTrimABI::ABI SoftFPABIs[] = {
      RVTrimABI::ABI_ILP32,
      RVTrimABI::ABI_ILP32E,
      RVTrimABI::ABI_LP64,
      RVTrimABI::ABI_LP64E,
  };

  RVTrimABI::ABI ABI = STI.getTargetABI();

  if (llvm::is_contained(SoftFPABIs, ABI) || !STI.hasStdExtF())
    return {};

  if (STI.hasStdExtQ())
    return ArrayRef(ArgFPR128s);

  if (STI.hasStdExtD())
    return ArrayRef(ArgFPR64s);

  return ArrayRef(ArgFPR32s);
}

ArrayRef<MCPhysReg> RVTrim::getArgVRs(const RVTrimSubtarget &STI) {
  if (STI.hasVInstructions())
    return ArrayRef(ArgVRs);

  return {};
}

static ArrayRef<MCPhysReg> getArgGPR16s(const RVTrimABI::ABI ABI) {
  // The GPRs used for passing arguments in the ILP32* and LP64* ABIs, except
  // the ILP32E ABI.
  static const MCPhysReg ArgIGPRs[] = {RVTrim::X10_H, RVTrim::X11_H, RVTrim::X12_H,
                                       RVTrim::X13_H, RVTrim::X14_H, RVTrim::X15_H,
                                       RVTrim::X16_H, RVTrim::X17_H};
  // The GPRs used for passing arguments in the ILP32E/LP64E ABI.
  static const MCPhysReg ArgEGPRs[] = {RVTrim::X10_H, RVTrim::X11_H,
                                       RVTrim::X12_H, RVTrim::X13_H,
                                       RVTrim::X14_H, RVTrim::X15_H};

  if (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E)
    return ArrayRef(ArgEGPRs);

  return ArrayRef(ArgIGPRs);
}

static ArrayRef<MCPhysReg> getArgGPR32s(const RVTrimABI::ABI ABI) {
  // The GPRs used for passing arguments in the ILP32* and LP64* ABIs, except
  // the ILP32E ABI.
  static const MCPhysReg ArgIGPRs[] = {RVTrim::X10_W, RVTrim::X11_W, RVTrim::X12_W,
                                       RVTrim::X13_W, RVTrim::X14_W, RVTrim::X15_W,
                                       RVTrim::X16_W, RVTrim::X17_W};
  // The GPRs used for passing arguments in the ILP32E/LP64E ABI.
  static const MCPhysReg ArgEGPRs[] = {RVTrim::X10_W, RVTrim::X11_W,
                                       RVTrim::X12_W, RVTrim::X13_W,
                                       RVTrim::X14_W, RVTrim::X15_W};

  if (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E)
    return ArrayRef(ArgEGPRs);

  return ArrayRef(ArgIGPRs);
}

static ArrayRef<MCPhysReg> getFastCCArgGPRs(const RVTrimABI::ABI ABI) {
  // The GPRs used for passing arguments in the FastCC, X5 and X6 might be used
  // for save-restore libcall, so we don't use them.
  // Don't use X7 for fastcc, since Zicfilp uses X7 as the label register.
  static const MCPhysReg FastCCIGPRs[] = {
      RVTrim::X10, RVTrim::X11, RVTrim::X12, RVTrim::X13, RVTrim::X14, RVTrim::X15,
      RVTrim::X16, RVTrim::X17, RVTrim::X28, RVTrim::X29, RVTrim::X30, RVTrim::X31};

  // The GPRs used for passing arguments in the FastCC when using ILP32E/LP64E.
  static const MCPhysReg FastCCEGPRs[] = {RVTrim::X10, RVTrim::X11, RVTrim::X12,
                                          RVTrim::X13, RVTrim::X14, RVTrim::X15};

  if (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E)
    return ArrayRef(FastCCEGPRs);

  return ArrayRef(FastCCIGPRs);
}

static ArrayRef<MCPhysReg> getFastCCArgGPRF16s(const RVTrimABI::ABI ABI) {
  // The GPRs used for passing arguments in the FastCC, X5 and X6 might be used
  // for save-restore libcall, so we don't use them.
  // Don't use X7 for fastcc, since Zicfilp uses X7 as the label register.
  static const MCPhysReg FastCCIGPRs[] = {
      RVTrim::X10_H, RVTrim::X11_H, RVTrim::X12_H, RVTrim::X13_H,
      RVTrim::X14_H, RVTrim::X15_H, RVTrim::X16_H, RVTrim::X17_H,
      RVTrim::X28_H, RVTrim::X29_H, RVTrim::X30_H, RVTrim::X31_H};

  // The GPRs used for passing arguments in the FastCC when using ILP32E/LP64E.
  static const MCPhysReg FastCCEGPRs[] = {RVTrim::X10_H, RVTrim::X11_H,
                                          RVTrim::X12_H, RVTrim::X13_H,
                                          RVTrim::X14_H, RVTrim::X15_H};

  if (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E)
    return ArrayRef(FastCCEGPRs);

  return ArrayRef(FastCCIGPRs);
}

static ArrayRef<MCPhysReg> getFastCCArgGPRF32s(const RVTrimABI::ABI ABI) {
  // The GPRs used for passing arguments in the FastCC, X5 and X6 might be used
  // for save-restore libcall, so we don't use them.
  // Don't use X7 for fastcc, since Zicfilp uses X7 as the label register.
  static const MCPhysReg FastCCIGPRs[] = {
      RVTrim::X10_W, RVTrim::X11_W, RVTrim::X12_W, RVTrim::X13_W,
      RVTrim::X14_W, RVTrim::X15_W, RVTrim::X16_W, RVTrim::X17_W,
      RVTrim::X28_W, RVTrim::X29_W, RVTrim::X30_W, RVTrim::X31_W};

  // The GPRs used for passing arguments in the FastCC when using ILP32E/LP64E.
  static const MCPhysReg FastCCEGPRs[] = {RVTrim::X10_W, RVTrim::X11_W,
                                          RVTrim::X12_W, RVTrim::X13_W,
                                          RVTrim::X14_W, RVTrim::X15_W};

  if (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E)
    return ArrayRef(FastCCEGPRs);

  return ArrayRef(FastCCIGPRs);
}

// Pass a 2*XLEN argument that has been split into two XLEN values through
// registers or the stack as necessary.
static bool CC_RVTrimAssign2XLen(CCState &State, CCValAssign VA1,
                                ISD::ArgFlagsTy ArgFlags1, unsigned ValNo2,
                                MVT ValVT2, MVT LocVT2,
                                ISD::ArgFlagsTy ArgFlags2,
                                const RVTrimSubtarget &Subtarget) {
  unsigned XLen = Subtarget.getXLen();
  unsigned XLenInBytes = XLen / 8;
  RVTrimABI::ABI ABI = Subtarget.getTargetABI();
  bool EABI = ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E;

  ArrayRef<MCPhysReg> ArgGPRs = RVTrim::getArgGPRs(Subtarget);

  if (MCRegister Reg = State.AllocateReg(ArgGPRs)) {
    // At least one half can be passed via register.
    State.addLoc(CCValAssign::getReg(VA1.getValNo(), VA1.getValVT(), Reg,
                                     VA1.getLocVT(), CCValAssign::Full));
  } else {
    // Both halves must be passed on the stack, with proper alignment.
    // TODO: To be compatible with GCC's behaviors, we force them to have 4-byte
    // alignment. This behavior may be changed when RV32E/ILP32E is ratified.
    Align StackAlign(XLenInBytes);
    if (!EABI || XLen != 32)
      StackAlign = std::max(StackAlign, ArgFlags1.getNonZeroOrigAlign());
    State.addLoc(
        CCValAssign::getMem(VA1.getValNo(), VA1.getValVT(),
                            State.AllocateStack(XLenInBytes, StackAlign),
                            VA1.getLocVT(), CCValAssign::Full));
    State.addLoc(CCValAssign::getMem(
        ValNo2, ValVT2, State.AllocateStack(XLenInBytes, Align(XLenInBytes)),
        LocVT2, CCValAssign::Full));
    return false;
  }

  if (MCRegister Reg = State.AllocateReg(ArgGPRs)) {
    // The second half can also be passed via register.
    State.addLoc(
        CCValAssign::getReg(ValNo2, ValVT2, Reg, LocVT2, CCValAssign::Full));
  } else {
    // The second half is passed via the stack, without additional alignment.
    State.addLoc(CCValAssign::getMem(
        ValNo2, ValVT2, State.AllocateStack(XLenInBytes, Align(XLenInBytes)),
        LocVT2, CCValAssign::Full));
  }

  return false;
}

static MCRegister allocateRVVReg(MVT LocVT, unsigned ValNo, CCState &State,
                                 const RVTrimTargetLowering &TLI) {
  const TargetRegisterClass *RC = TLI.getRegClassFor(LocVT);
  if (RC == &RVTrim::VRRegClass) {
    // Assign the first mask argument to V0.
    // This is an interim calling convention and it may be changed in the
    // future.
    if (LocVT.getVectorElementType() == MVT::i1)
      if (MCRegister Reg = State.AllocateReg(RVTrim::V0))
        return Reg;
    return State.AllocateReg(ArgVRs);
  }
  if (RC == &RVTrim::VRM2RegClass)
    return State.AllocateReg(ArgVRM2s);
  if (RC == &RVTrim::VRM4RegClass)
    return State.AllocateReg(ArgVRM4s);
  if (RC == &RVTrim::VRM8RegClass)
    return State.AllocateReg(ArgVRM8s);
  if (RC == &RVTrim::VRN2M1RegClass)
    return State.AllocateReg(ArgVRN2M1s);
  if (RC == &RVTrim::VRN3M1RegClass)
    return State.AllocateReg(ArgVRN3M1s);
  if (RC == &RVTrim::VRN4M1RegClass)
    return State.AllocateReg(ArgVRN4M1s);
  if (RC == &RVTrim::VRN5M1RegClass)
    return State.AllocateReg(ArgVRN5M1s);
  if (RC == &RVTrim::VRN6M1RegClass)
    return State.AllocateReg(ArgVRN6M1s);
  if (RC == &RVTrim::VRN7M1RegClass)
    return State.AllocateReg(ArgVRN7M1s);
  if (RC == &RVTrim::VRN8M1RegClass)
    return State.AllocateReg(ArgVRN8M1s);
  if (RC == &RVTrim::VRN2M2RegClass)
    return State.AllocateReg(ArgVRN2M2s);
  if (RC == &RVTrim::VRN3M2RegClass)
    return State.AllocateReg(ArgVRN3M2s);
  if (RC == &RVTrim::VRN4M2RegClass)
    return State.AllocateReg(ArgVRN4M2s);
  if (RC == &RVTrim::VRN2M4RegClass)
    return State.AllocateReg(ArgVRN2M4s);
  llvm_unreachable("Unhandled register class for ValueType");
}

// Implements the RISC-V calling convention. Returns true upon failure.
//
// This has a slightly different signature to CCAssignFn - it adds `bool IsRet`.
static bool CC_RVTrim_Impl(unsigned ValNo, MVT ValVT, MVT LocVT,
                          CCValAssign::LocInfo LocInfo,
                          ISD::ArgFlagsTy ArgFlags, Type *OrigTy,
                          CCState &State, bool IsRet) {
  assert(ValVT == LocVT && "Expected ValVT and LocVT to match");
  const MachineFunction &MF = State.getMachineFunction();
  const DataLayout &DL = MF.getDataLayout();
  const RVTrimSubtarget &Subtarget = MF.getSubtarget<RVTrimSubtarget>();
  const RVTrimTargetLowering &TLI = *Subtarget.getTargetLowering();

  unsigned XLen = Subtarget.getXLen();
  MVT XLenVT = Subtarget.getXLenVT();

  if (ArgFlags.isNest()) {
    // Static chain parameter must not be passed in normal argument registers,
    // so we assign t2/t3 for it as done in GCC's
    // __builtin_call_with_static_chain
    bool HasCFBranch =
        MF.getInfo<RVTrimMachineFunctionInfo>()->hasCFProtectionBranch();

    // Normal: t2, Branch control flow protection: t3
    const auto StaticChainReg = HasCFBranch ? RVTrim::X28 : RVTrim::X7;

    RVTrimABI::ABI ABI = Subtarget.getTargetABI();
    if (HasCFBranch &&
        (ABI == RVTrimABI::ABI_ILP32E || ABI == RVTrimABI::ABI_LP64E))
      reportFatalUsageError(
          "Nested functions with control flow protection are not "
          "usable with ILP32E or LP64E ABI.");
    if (MCRegister Reg = State.AllocateReg(StaticChainReg)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  // Any return value split in to more than two values can't be returned
  // directly. Vectors are returned via the available vector registers.
  if ((!LocVT.isVector() || Subtarget.isPExtPackedType(LocVT)) && IsRet &&
      ValNo > 1)
    return true;

  // Double wide packed types require 2 GPRs so we can only return 1 of them.
  if (Subtarget.isPExtPackedDoubleType(LocVT) && IsRet && ValNo > 0)
    return true;

  // AllowFPRForF16_F32 if targeting an FLEN>=32 ABI and the argument isn't
  // variadic.
  bool AllowFPRForF16_F32 = false;
  // UseFPRForF64 if targeting an FLEN>=64 ABI and the argument isn't variadic.
  bool AllowFPRForF64 = false;

  RVTrimABI::ABI ABI = Subtarget.getTargetABI();
  switch (ABI) {
  default:
    llvm_unreachable("Unexpected ABI");
  case RVTrimABI::ABI_ILP32:
  case RVTrimABI::ABI_ILP32E:
  case RVTrimABI::ABI_LP64:
  case RVTrimABI::ABI_LP64E:
    break;
  case RVTrimABI::ABI_ILP32D:
  case RVTrimABI::ABI_LP64D:
    AllowFPRForF64 = !ArgFlags.isVarArg();
    [[fallthrough]];
  case RVTrimABI::ABI_ILP32F:
  case RVTrimABI::ABI_LP64F:
    AllowFPRForF16_F32 = !ArgFlags.isVarArg();
    break;
  }

  if ((LocVT == MVT::f16 || LocVT == MVT::bf16) && AllowFPRForF16_F32) {
    if (MCRegister Reg = State.AllocateReg(ArgFPR16s)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f32 && AllowFPRForF16_F32) {
    if (MCRegister Reg = State.AllocateReg(ArgFPR32s)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f64 && AllowFPRForF64) {
    if (MCRegister Reg = State.AllocateReg(ArgFPR64s)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f16 && Subtarget.hasStdExtZhinxmin()) {
    if (MCRegister Reg = State.AllocateReg(getArgGPR16s(ABI))) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f32 && Subtarget.hasStdExtZfinx()) {
    if (MCRegister Reg = State.AllocateReg(getArgGPR32s(ABI))) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  ArrayRef<MCPhysReg> ArgGPRs = RVTrim::getArgGPRs(Subtarget);

  // Zdinx use GPR without a bitcast when possible.
  if (LocVT == MVT::f64 && XLen == 64 && Subtarget.hasStdExtZdinx()) {
    if (MCRegister Reg = State.AllocateReg(ArgGPRs)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  // FP smaller than XLen, uses custom GPR.
  if (LocVT == MVT::f16 || LocVT == MVT::bf16 ||
      (LocVT == MVT::f32 && XLen == 64)) {
    if (MCRegister Reg = State.AllocateReg(ArgGPRs)) {
      LocVT = XLenVT;
      State.addLoc(
          CCValAssign::getCustomReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  // Bitcast FP to GPR if we can use a GPR register.
  if ((XLen == 32 && LocVT == MVT::f32) || (XLen == 64 && LocVT == MVT::f64)) {
    if (MCRegister Reg = State.AllocateReg(ArgGPRs)) {
      LocVT = XLenVT;
      LocInfo = CCValAssign::BCvt;
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  // If this is a variadic argument, the RISC-V calling convention requires
  // that it is assigned an 'even' or 'aligned' register if it has 8-byte
  // alignment (RV32) or 16-byte alignment (RV64). An aligned register should
  // be used regardless of whether the original argument was split during
  // legalisation or not. The argument will not be passed by registers if the
  // original type is larger than 2*XLEN, so the register alignment rule does
  // not apply.
  // TODO: To be compatible with GCC's behaviors, we don't align registers
  // currently if we are using ILP32E calling convention. This behavior may be
  // changed when RV32E/ILP32E is ratified.
  unsigned TwoXLenInBytes = (2 * XLen) / 8;
  if (ArgFlags.isVarArg() && ArgFlags.getNonZeroOrigAlign() == TwoXLenInBytes &&
      DL.getTypeAllocSize(OrigTy) == TwoXLenInBytes &&
      ABI != RVTrimABI::ABI_ILP32E) {
    unsigned RegIdx = State.getFirstUnallocated(ArgGPRs);
    // Skip 'odd' register if necessary.
    if (RegIdx != std::size(ArgGPRs) && RegIdx % 2 == 1)
      State.AllocateReg(ArgGPRs);
  }

  SmallVectorImpl<CCValAssign> &PendingLocs = State.getPendingLocs();
  SmallVectorImpl<ISD::ArgFlagsTy> &PendingArgFlags =
      State.getPendingArgFlags();

  assert(PendingLocs.size() == PendingArgFlags.size() &&
         "PendingLocs and PendingArgFlags out of sync");

  // Handle passing f64 on RV32D with a soft float ABI or when floating point
  // registers are exhausted. Or 64-bit P extension vectors on RV32.
  if (XLen == 32 &&
      (LocVT == MVT::f64 || (Subtarget.isPExtPackedDoubleType(LocVT) &&
                             !ArgFlags.isSplit() && PendingLocs.empty()))) {
    assert(PendingLocs.empty() &&
           "Can't lower f64 or P extension vector if it is split");
    // Depending on available argument GPRS, f64 may be passed in a pair of
    // GPRs, split between a GPR and the stack, or passed completely on the
    // stack. LowerCall/LowerFormalArguments/LowerReturn must recognise these
    // cases.
    MCRegister Reg = State.AllocateReg(ArgGPRs);
    if (!Reg) {
      int64_t StackOffset = State.AllocateStack(8, Align(8));
      State.addLoc(
          CCValAssign::getMem(ValNo, ValVT, StackOffset, LocVT, LocInfo));
      return false;
    }
    LocVT = MVT::i32;
    State.addLoc(CCValAssign::getCustomReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    MCRegister HiReg = State.AllocateReg(ArgGPRs);
    if (HiReg) {
      State.addLoc(
          CCValAssign::getCustomReg(ValNo, ValVT, HiReg, LocVT, LocInfo));
    } else {
      int64_t StackOffset = State.AllocateStack(4, Align(4));
      State.addLoc(
          CCValAssign::getCustomMem(ValNo, ValVT, StackOffset, LocVT, LocInfo));
    }
    return false;
  }

  // If the split argument only had two elements, it should be passed directly
  // in registers or on the stack.
  if ((LocVT.isScalarInteger() ||
       (Subtarget.isPExtPackedType(LocVT) && LocVT.getSizeInBits() == XLen)) &&
      ArgFlags.isSplitEnd() && PendingLocs.size() <= 1) {
    assert(PendingLocs.size() == 1 && "Unexpected PendingLocs.size()");
    // Apply the normal calling convention rules to the first half of the
    // split argument.
    CCValAssign VA = PendingLocs[0];
    ISD::ArgFlagsTy AF = PendingArgFlags[0];
    PendingLocs.clear();
    PendingArgFlags.clear();
    return CC_RVTrimAssign2XLen(State, VA, AF, ValNo, ValVT, LocVT, ArgFlags,
                               Subtarget);
  }

  // Split arguments might be passed indirectly, so keep track of the pending
  // values. Split vectors excluding P extension packed vectors(see
  // isPExtPackedType) are passed via a mix of registers and indirectly, so
  // treat them as we would any other argument.
  if ((LocVT.isScalarInteger() || Subtarget.isPExtPackedType(LocVT)) &&
      (ArgFlags.isSplit() || !PendingLocs.empty())) {
    PendingLocs.push_back(
        CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));
    PendingArgFlags.push_back(ArgFlags);
    if (!ArgFlags.isSplitEnd()) {
      return false;
    }
  }

  // Allocate to a register if possible, or else a stack slot.
  MCRegister Reg;
  unsigned StoreSizeBytes = XLen / 8;
  Align StackAlign = Align(XLen / 8);

  // FIXME: If P extension and V extension are enabled at the same time,
  // who should go first?
  if (!Subtarget.isPExtPackedType(LocVT) &&
      (LocVT.isVector() || LocVT.isRISCVVectorTuple())) {
    Reg = allocateRVVReg(LocVT, ValNo, State, TLI);
    if (Reg) {
      // Fixed-length vectors are located in the corresponding scalable-vector
      // container types.
      if (LocVT.isFixedLengthVector()) {
        LocVT = TLI.getContainerForFixedLengthVector(LocVT);
        State.addLoc(
            CCValAssign::getCustomReg(ValNo, ValVT, Reg, LocVT, LocInfo));
        return false;
      }
    } else {
      // For return values, the vector must be passed fully via registers or
      // via the stack.
      if (IsRet)
        return true;
      // Try using a GPR to pass the address
      if ((Reg = State.AllocateReg(ArgGPRs))) {
        LocVT = XLenVT;
        LocInfo = CCValAssign::Indirect;
      } else if (LocVT.isScalableVector()) {
        LocVT = XLenVT;
        LocInfo = CCValAssign::Indirect;
      } else {
        StoreSizeBytes = LocVT.getStoreSize();
        // Align vectors to their element sizes, being careful for vXi1
        // vectors.
        StackAlign = MaybeAlign(LocVT.getScalarSizeInBits() / 8).valueOrOne();
      }
    }
  } else {
    Reg = State.AllocateReg(ArgGPRs);
  }

  int64_t StackOffset =
      Reg ? 0 : State.AllocateStack(StoreSizeBytes, StackAlign);

  // If we reach this point and PendingLocs is non-empty, we must be at the
  // end of a split argument that must be passed indirectly.
  if (!PendingLocs.empty()) {
    assert(ArgFlags.isSplitEnd() && "Expected ArgFlags.isSplitEnd()");
    assert(PendingLocs.size() > 1 && "Unexpected PendingLocs.size()");

    for (auto &It : PendingLocs) {
      if (Reg)
        State.addLoc(CCValAssign::getReg(It.getValNo(), It.getValVT(), Reg,
                                         XLenVT, CCValAssign::Indirect));
      else
        State.addLoc(CCValAssign::getMem(It.getValNo(), It.getValVT(),
                                         StackOffset, XLenVT,
                                         CCValAssign::Indirect));
    }
    PendingLocs.clear();
    PendingArgFlags.clear();
    return false;
  }

  assert(((LocVT.isFloatingPoint() && !LocVT.isVector()) || LocVT == XLenVT ||
          Subtarget.isPExtPackedType(LocVT) ||
          (TLI.getSubtarget().hasVInstructions() &&
           (LocVT.isVector() || LocVT.isRISCVVectorTuple()))) &&
         "Expected an XLenVT or vector types at this stage");

  if (Reg) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  State.addLoc(CCValAssign::getMem(ValNo, ValVT, StackOffset, LocVT, LocInfo));
  return false;
}

// FastCC has less than 1% performance improvement for some particular
// benchmark. But theoretically, it may have benefit for some cases.
static bool CC_RVTrim_FastCC(unsigned ValNo, MVT ValVT, MVT LocVT,
                            CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, Type *OrigTy,
                            CCState &State) {
  const MachineFunction &MF = State.getMachineFunction();
  const RVTrimSubtarget &Subtarget = MF.getSubtarget<RVTrimSubtarget>();
  const RVTrimTargetLowering &TLI = *Subtarget.getTargetLowering();
  RVTrimABI::ABI ABI = Subtarget.getTargetABI();

  if ((LocVT == MVT::f16 && Subtarget.hasStdExtZfhmin()) ||
      (LocVT == MVT::bf16 && Subtarget.hasStdExtZfbfmin())) {
    static const MCPhysReg FPR16List[] = {
        RVTrim::F10_H, RVTrim::F11_H, RVTrim::F12_H, RVTrim::F13_H, RVTrim::F14_H,
        RVTrim::F15_H, RVTrim::F16_H, RVTrim::F17_H, RVTrim::F0_H,  RVTrim::F1_H,
        RVTrim::F2_H,  RVTrim::F3_H,  RVTrim::F4_H,  RVTrim::F5_H,  RVTrim::F6_H,
        RVTrim::F7_H,  RVTrim::F28_H, RVTrim::F29_H, RVTrim::F30_H, RVTrim::F31_H};
    if (MCRegister Reg = State.AllocateReg(FPR16List)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f32 && Subtarget.hasStdExtF()) {
    static const MCPhysReg FPR32List[] = {
        RVTrim::F10_F, RVTrim::F11_F, RVTrim::F12_F, RVTrim::F13_F, RVTrim::F14_F,
        RVTrim::F15_F, RVTrim::F16_F, RVTrim::F17_F, RVTrim::F0_F,  RVTrim::F1_F,
        RVTrim::F2_F,  RVTrim::F3_F,  RVTrim::F4_F,  RVTrim::F5_F,  RVTrim::F6_F,
        RVTrim::F7_F,  RVTrim::F28_F, RVTrim::F29_F, RVTrim::F30_F, RVTrim::F31_F};
    if (MCRegister Reg = State.AllocateReg(FPR32List)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f64 && Subtarget.hasStdExtD()) {
    static const MCPhysReg FPR64List[] = {
        RVTrim::F10_D, RVTrim::F11_D, RVTrim::F12_D, RVTrim::F13_D, RVTrim::F14_D,
        RVTrim::F15_D, RVTrim::F16_D, RVTrim::F17_D, RVTrim::F0_D,  RVTrim::F1_D,
        RVTrim::F2_D,  RVTrim::F3_D,  RVTrim::F4_D,  RVTrim::F5_D,  RVTrim::F6_D,
        RVTrim::F7_D,  RVTrim::F28_D, RVTrim::F29_D, RVTrim::F30_D, RVTrim::F31_D};
    if (MCRegister Reg = State.AllocateReg(FPR64List)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  MVT XLenVT = Subtarget.getXLenVT();

  // Check if there is an available GPRF16 before hitting the stack.
  if ((LocVT == MVT::f16 && Subtarget.hasStdExtZhinxmin())) {
    if (MCRegister Reg = State.AllocateReg(getFastCCArgGPRF16s(ABI))) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  // Check if there is an available GPRF32 before hitting the stack.
  if (LocVT == MVT::f32 && Subtarget.hasStdExtZfinx()) {
    if (MCRegister Reg = State.AllocateReg(getFastCCArgGPRF32s(ABI))) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  // Check if there is an available GPR before hitting the stack.
  if (LocVT == MVT::f64 && Subtarget.is64Bit() && Subtarget.hasStdExtZdinx()) {
    if (MCRegister Reg = State.AllocateReg(getFastCCArgGPRs(ABI))) {
      if (LocVT.getSizeInBits() != Subtarget.getXLen()) {
        LocVT = XLenVT;
        State.addLoc(
            CCValAssign::getCustomReg(ValNo, ValVT, Reg, LocVT, LocInfo));
        return false;
      }
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  ArrayRef<MCPhysReg> ArgGPRs = getFastCCArgGPRs(ABI);

  if (LocVT.isVector()) {
    if (MCRegister Reg = allocateRVVReg(ValVT, ValNo, State, TLI)) {
      // Fixed-length vectors are located in the corresponding scalable-vector
      // container types.
      if (LocVT.isFixedLengthVector()) {
        LocVT = TLI.getContainerForFixedLengthVector(LocVT);
        State.addLoc(
            CCValAssign::getCustomReg(ValNo, ValVT, Reg, LocVT, LocInfo));
        return false;
      }
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }

    // Pass scalable vectors indirectly. Pass fixed vectors indirectly if we
    // have a free GPR.
    if (LocVT.isScalableVector() ||
        State.getFirstUnallocated(ArgGPRs) != ArgGPRs.size()) {
      LocInfo = CCValAssign::Indirect;
      LocVT = XLenVT;
    }
  }

  if (LocVT == XLenVT) {
    if (MCRegister Reg = State.AllocateReg(getFastCCArgGPRs(ABI))) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == XLenVT || LocVT == MVT::f16 || LocVT == MVT::bf16 ||
      LocVT == MVT::f32 || LocVT == MVT::f64 || LocVT.isFixedLengthVector()) {
    Align StackAlign = MaybeAlign(ValVT.getScalarSizeInBits() / 8).valueOrOne();
    int64_t Offset = State.AllocateStack(LocVT.getStoreSize(), StackAlign);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }

  return true; // CC didn't match.
}

static bool CC_RVTrim_GHC(unsigned ValNo, MVT ValVT, MVT LocVT,
                         CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags,
                         Type *OrigTy, CCState &State) {
  if (ArgFlags.isNest()) {
    report_fatal_error(
        "Attribute 'nest' is not supported in GHC calling convention");
  }

  static const MCPhysReg GPRList[] = {
      RVTrim::X9,  RVTrim::X18, RVTrim::X19, RVTrim::X20, RVTrim::X21, RVTrim::X22,
      RVTrim::X23, RVTrim::X24, RVTrim::X25, RVTrim::X26, RVTrim::X27};

  if (LocVT == MVT::i32 || LocVT == MVT::i64) {
    // Pass in STG registers: Base, Sp, Hp, R1, R2, R3, R4, R5, R6, R7, SpLim
    //                        s1    s2  s3  s4  s5  s6  s7  s8  s9  s10 s11
    if (MCRegister Reg = State.AllocateReg(GPRList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  const RVTrimSubtarget &Subtarget =
      State.getMachineFunction().getSubtarget<RVTrimSubtarget>();

  if (LocVT == MVT::f32 && Subtarget.hasStdExtF()) {
    // Pass in STG registers: F1, ..., F6
    //                        fs0 ... fs5
    static const MCPhysReg FPR32List[] = {RVTrim::F8_F,  RVTrim::F9_F,
                                          RVTrim::F18_F, RVTrim::F19_F,
                                          RVTrim::F20_F, RVTrim::F21_F};
    if (MCRegister Reg = State.AllocateReg(FPR32List)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f64 && Subtarget.hasStdExtD()) {
    // Pass in STG registers: D1, ..., D6
    //                        fs6 ... fs11
    static const MCPhysReg FPR64List[] = {RVTrim::F22_D, RVTrim::F23_D,
                                          RVTrim::F24_D, RVTrim::F25_D,
                                          RVTrim::F26_D, RVTrim::F27_D};
    if (MCRegister Reg = State.AllocateReg(FPR64List)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f32 && Subtarget.hasStdExtZfinx()) {
    static const MCPhysReg GPR32List[] = {
        RVTrim::X9_W,  RVTrim::X18_W, RVTrim::X19_W, RVTrim::X20_W,
        RVTrim::X21_W, RVTrim::X22_W, RVTrim::X23_W, RVTrim::X24_W,
        RVTrim::X25_W, RVTrim::X26_W, RVTrim::X27_W};
    if (MCRegister Reg = State.AllocateReg(GPR32List)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  if (LocVT == MVT::f64 && Subtarget.hasStdExtZdinx() && Subtarget.is64Bit()) {
    if (MCRegister Reg = State.AllocateReg(GPRList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  report_fatal_error("No registers left in GHC calling convention");
  return true;
}
