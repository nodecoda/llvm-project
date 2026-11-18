//===-- GPUMinInstrInfo.cpp - GPUMin instruction information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinInstrInfo.h"
#include "GPUMinSubtarget.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "GPUMinGenInstrInfo.inc"

void GPUMinInstrInfo::anchor() {}

GPUMinInstrInfo::GPUMinInstrInfo(const GPUMinSubtarget &STI)
    : GPUMinGenInstrInfo(STI, RI), RI() {}
