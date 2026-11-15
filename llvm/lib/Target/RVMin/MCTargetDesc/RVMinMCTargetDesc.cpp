//===-- RVMinMCTargetDesc.cpp - RVMin target descriptions -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinMCTargetDesc.h"
#include "RVMinInstPrinter.h"
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
  // The second argument is the return-address register, used as the DWARF
  // "return address column" of every CIE.  Chapter 2 had no `ra` yet and
  // passed x0; since chapter 8 `ra` is a normal register.
  InitRVMinMCRegisterInfo(X, RVMin::X1);
  return X;
}

static MCAsmInfo *createRVMinMCAsmInfo(const MCRegisterInfo &MRI,
                                       const Triple &TT,
                                       const MCTargetOptions &Options) {
  MCAsmInfo *MAI = new RVMinMCAsmInfo(TT, Options);
  // Per-function CFI starts from "CFA = sp + 0"; the frame lowering then
  // updates the offset after `sp -= N`.  Without this the CIE has no
  // DW_CFA_def_cfa rule at all and unwinders cannot find the frame.
  unsigned SP = MRI.getDwarfRegNum(RVMin::X2, /*IsEH=*/true);
  MAI->addInitialFrameState(MCCFIInstruction::cfiDefCfa(nullptr, SP, 0));
  return MAI;
}

static MCSubtargetInfo *createRVMinMCSubtargetInfo(const Triple &TT,
                                                   StringRef CPU, StringRef FS) {
  return createRVMinMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCTargetStreamer *createRVMinNullTargetStreamer(MCStreamer &S) {
  return new MCTargetStreamer(S);
}

static MCInstPrinter *createRVMinMCInstPrinter(const Triple &T,
                                               unsigned SyntaxVariant,
                                               const MCAsmInfo &MAI,
                                               const MCInstrInfo &MII,
                                               const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new RVMinInstPrinter(MAI, MII, MRI);
  return nullptr;
}

static MCCodeEmitter *createRVMinMCCodeEmitterWrapper(const MCInstrInfo &MII,
                                                      MCContext &Ctx) {
  return createRVMinMCCodeEmitter(Ctx, MII);
}

static MCAsmBackend *
createRVMinMCAsmBackendWrapper(const Target &T, const MCSubtargetInfo &STI,
                               const MCRegisterInfo &MRI,
                               const MCTargetOptions &Options) {
  return createRVMinMCAsmBackend(T, STI, MRI, Options);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVMinTargetMC() {
  Target &T = getTheRVMinTarget();

  TargetRegistry::RegisterMCAsmInfo(T, createRVMinMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createRVMinMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createRVMinMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createRVMinMCSubtargetInfo);
  TargetRegistry::RegisterMCAsmBackend(T, createRVMinMCAsmBackendWrapper);
  TargetRegistry::RegisterMCCodeEmitter(T, createRVMinMCCodeEmitterWrapper);
  TargetRegistry::RegisterMCInstPrinter(T, createRVMinMCInstPrinter);
  TargetRegistry::RegisterNullTargetStreamer(T, createRVMinNullTargetStreamer);
}
