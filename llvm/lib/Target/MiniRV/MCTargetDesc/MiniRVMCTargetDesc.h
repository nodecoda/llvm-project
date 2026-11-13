//===-- MiniRVMCTargetDesc.h - MiniRV target descriptions ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MCTARGETDESC_MINIRVMCTARGETDESC_H
#define LLVM_LIB_TARGET_MINIRV_MCTARGETDESC_MINIRVMCTARGETDESC_H

#include <cstdint>
#include <memory>

namespace llvm {

class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;
class Triple;

MCCodeEmitter *createMiniRVMCCodeEmitter(const MCInstrInfo &MCII,
                                         MCContext &Ctx);
MCAsmBackend *createMiniRVAsmBackend(const Target &T,
                                     const MCSubtargetInfo &STI,
                                     const MCRegisterInfo &MRI,
                                     const MCTargetOptions &Options);
std::unique_ptr<MCObjectTargetWriter> createMiniRVELFObjectWriter(uint8_t OSABI);

} // namespace llvm

#define GET_REGINFO_ENUM
#include "MiniRVGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#include "MiniRVGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "MiniRVGenSubtargetInfo.inc"

#endif
