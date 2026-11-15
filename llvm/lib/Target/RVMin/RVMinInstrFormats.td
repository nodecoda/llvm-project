//===-- RVMinInstrFormats.td - RVMin instruction formats ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

class RVMinInst<dag outs, dag ins, string asmstr, list<dag> pattern>
    : Instruction {
  field bits<32> Inst;
  let Namespace = "RVMin";
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
  let Size = 4;
}

class RVMinPseudo<dag outs, dag ins, string asmstr, list<dag> pattern>
    : RVMinInst<outs, ins, asmstr, pattern> {
  let isPseudo = 1;
  let Size = 0;
}
