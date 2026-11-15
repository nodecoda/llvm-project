//===-- RVMinInstrInfo.td - RVMin instruction definitions ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 0 only needs the call-frame pseudos so that TargetInstrInfo can be
// constructed.  Real RV32I instructions are added in chapter 1.
//
//===----------------------------------------------------------------------===//

include "RVMinInstrFormats.td"

def ADJCALLSTACKDOWN : RVMinPseudo<(outs), (ins i32imm:$amt), "", []>;
def ADJCALLSTACKUP : RVMinPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2), "", []>;
