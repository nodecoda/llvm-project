//===-- MiniRVMCTargetDesc.cpp - MiniRV target descriptions ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVMCTargetDesc.h"
#include "MiniRVInstPrinter.h"
#include "MiniRVMCAsmInfo.h"
#include "TargetInfo/MiniRVTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "MiniRVGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "MiniRVGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "MiniRVGenRegisterInfo.inc"

static MCInstrInfo *createMiniRVMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitMiniRVMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createMiniRVMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitMiniRVMCRegisterInfo(X, MiniRV::X1);
  return X;
}

static MCAsmInfo *createMiniRVMCAsmInfo(const MCRegisterInfo &MRI,
                                        const Triple &TT,
                                        const MCTargetOptions &Options) {
  return new MiniRVMCAsmInfo(TT, Options);
}

static MCSubtargetInfo *createMiniRVMCSubtargetInfo(const Triple &TT,
                                                    StringRef CPU,
                                                    StringRef FS) {
  return createMiniRVMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCInstPrinter *createMiniRVMCInstPrinter(const Triple &T,
                                                unsigned SyntaxVariant,
                                                const MCAsmInfo &MAI,
                                                const MCInstrInfo &MII,
                                                const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new MiniRVInstPrinter(MAI, MII, MRI);
  return nullptr;
}

static MCTargetStreamer *createNullTargetStreamer(MCStreamer &S) {
  return new MCTargetStreamer(S);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMiniRVTargetMC() {
  Target &T = getTheMiniRVTarget();

  TargetRegistry::RegisterMCAsmInfo(T, createMiniRVMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createMiniRVMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createMiniRVMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createMiniRVMCSubtargetInfo);
  TargetRegistry::RegisterMCInstPrinter(T, createMiniRVMCInstPrinter);
  TargetRegistry::RegisterMCCodeEmitter(T, createMiniRVMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createMiniRVAsmBackend);
  TargetRegistry::RegisterNullTargetStreamer(T, createNullTargetStreamer);
}

extern "C" void LLVMInitializeMiniRVTargetInfo();

// This target has no TargetMachine, so LLVMInitializeMiniRVTarget (referenced
// from Targets.def) is the MC entry point rather than a CodeGen one.
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeMiniRVTarget() {
  LLVMInitializeMiniRVTargetInfo();
  LLVMInitializeMiniRVTargetMC();
}
