//===-- RVMinInstrInfo.cpp - RVMin instruction information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinInstrInfo.h"
#include "RVMin.h"
#include "RVMinSubtarget.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "RVMinGenInstrInfo.inc"

void RVMinInstrInfo::anchor() {}

RVMinInstrInfo::RVMinInstrInfo(const RVMinSubtarget &STI)
    : RVMinGenInstrInfo(STI, RI, RVMin::ADJCALLSTACKDOWN,
                        RVMin::ADJCALLSTACKUP),
      RI() {}
