//===-- GPUMinMCTargetDesc.cpp - GPUMin target descriptions ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 00: wire the GPUMin target into the MC layer.  This file is the
// single place where llvm-mc, llc and llvm-objdump learn about the target's
// instruction printer, encoder, assembler backend and ELF writer.
//
//===----------------------------------------------------------------------===//

#include "GPUMinMCTargetDesc.h"
#include "GPUMinInstPrinter.h"
#include "GPUMinMCAsmInfo.h"
#include "TargetInfo/GPUMinTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "GPUMinGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "GPUMinGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "GPUMinGenRegisterInfo.inc"

static MCInstrInfo *createGPUMinMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitGPUMinMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createGPUMinMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  // The second argument is the return-address register: r31 (lr) becomes the
  // DWARF return-address column of every CIE.
  InitGPUMinMCRegisterInfo(X, GPUMin::R31);
  return X;
}

static MCAsmInfo *createGPUMinMCAsmInfo(const MCRegisterInfo &MRI,
                                        const Triple &TT,
                                        const MCTargetOptions &Options) {
  return new GPUMinMCAsmInfo(TT, Options);
}

static MCSubtargetInfo *createGPUMinMCSubtargetInfo(const Triple &TT,
                                                    StringRef CPU, StringRef FS) {
  return createGPUMinMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCTargetStreamer *createGPUMinNullTargetStreamer(MCStreamer &S) {
  return new MCTargetStreamer(S);
}

static MCInstPrinter *createGPUMinMCInstPrinter(const Triple &T,
                                                unsigned SyntaxVariant,
                                                const MCAsmInfo &MAI,
                                                const MCInstrInfo &MII,
                                                const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new GPUMinInstPrinter(MAI, MII, MRI);
  return nullptr;
}

static MCCodeEmitter *createGPUMinMCCodeEmitterWrapper(const MCInstrInfo &MII,
                                                       MCContext &Ctx) {
  return createGPUMinMCCodeEmitter(Ctx, MII);
}

static MCAsmBackend *
createGPUMinMCAsmBackendWrapper(const Target &T, const MCSubtargetInfo &STI,
                                const MCRegisterInfo &MRI,
                                const MCTargetOptions &Options) {
  return createGPUMinMCAsmBackend(T, STI, MRI, Options);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUMinTargetMC() {
  Target &T = getTheGPUMinTarget();

  TargetRegistry::RegisterMCAsmInfo(T, createGPUMinMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createGPUMinMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createGPUMinMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createGPUMinMCSubtargetInfo);
  TargetRegistry::RegisterMCAsmBackend(T, createGPUMinMCAsmBackendWrapper);
  TargetRegistry::RegisterMCCodeEmitter(T, createGPUMinMCCodeEmitterWrapper);
  TargetRegistry::RegisterMCInstPrinter(T, createGPUMinMCInstPrinter);
  TargetRegistry::RegisterNullTargetStreamer(T, createGPUMinNullTargetStreamer);
}
