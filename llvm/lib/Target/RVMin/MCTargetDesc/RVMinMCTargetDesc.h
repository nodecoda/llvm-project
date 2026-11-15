//===-- RVMinMCTargetDesc.h - RVMin target descriptions --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINMCTARGETDESC_H
#define LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINMCTARGETDESC_H

#include "llvm/Support/DataTypes.h"

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

MCCodeEmitter *createRVMinMCCodeEmitter(MCContext &Ctx,
                                        const MCInstrInfo &MCII);

MCAsmBackend *createRVMinMCAsmBackend(const Target &T,
                                      const MCSubtargetInfo &STI,
                                      const MCRegisterInfo &MRI,
                                      const MCTargetOptions &Options);

std::unique_ptr<MCObjectTargetWriter>
createRVMinELFObjectWriter(uint8_t OSABI);
} // namespace llvm

#define GET_REGINFO_ENUM
#include "RVMinGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "RVMinGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "RVMinGenSubtargetInfo.inc"

#endif // LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINMCTARGETDESC_H
