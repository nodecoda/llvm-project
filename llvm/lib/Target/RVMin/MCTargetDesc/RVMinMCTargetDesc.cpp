//===-- RVMinMCTargetDesc.cpp - RVMin target descriptions -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinMCTargetDesc.h"
#include "RVMinMCAsmInfo.h"
#include "TargetInfo/RVMinTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "RVMinGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "RVMinGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "RVMinGenRegisterInfo.inc"

static MCInstrInfo *createRVMinMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitRVMinMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createRVMinMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitRVMinMCRegisterInfo(X, RVMin::X0);
  return X;
}

static MCAsmInfo *createRVMinMCAsmInfo(const MCRegisterInfo &MRI,
                                       const Triple &TT,
                                       const MCTargetOptions &Options) {
  return new RVMinMCAsmInfo(TT, Options);
}

static MCSubtargetInfo *createRVMinMCSubtargetInfo(const Triple &TT,
                                                   StringRef CPU, StringRef FS) {
  return createRVMinMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCTargetStreamer *createRVMinNullTargetStreamer(MCStreamer &S) {
  return new MCTargetStreamer(S);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinTargetMC() {
  Target &T = getTheRVMinTarget();

  TargetRegistry::RegisterMCAsmInfo(T, createRVMinMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createRVMinMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createRVMinMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createRVMinMCSubtargetInfo);
  TargetRegistry::RegisterNullTargetStreamer(T, createRVMinNullTargetStreamer);
}
