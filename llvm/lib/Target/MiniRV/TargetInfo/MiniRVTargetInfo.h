//===-- MiniRVTargetInfo.h - MiniRV target implementation -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_TARGETINFO_MINIRVTARGETINFO_H
#define LLVM_LIB_TARGET_MINIRV_TARGETINFO_MINIRVTARGETINFO_H

namespace llvm {

class Target;

Target &getTheMiniRVTarget();

} // namespace llvm

#endif
