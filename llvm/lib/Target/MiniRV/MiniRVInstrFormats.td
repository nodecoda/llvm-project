//===-- MiniRVInstrFormats.td - MiniRV instruction formats --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Kept in sync with MiniRVBaseInfo.h.
class InstFormat<bits<5> val> {
  bits<5> Value = val;
}
def InstFormatR : InstFormat<1>;
def InstFormatI : InstFormat<3>;
def InstFormatS : InstFormat<4>;
def InstFormatB : InstFormat<5>;
def InstFormatU : InstFormat<6>;
def InstFormatJ : InstFormat<7>;

// The base class auto-td records derive from.  It deliberately declares no
// encoding bits: every Inst bit of a generated record comes from the opcode
// database.
class MiniRVInst<dag outs, dag ins, string opcodestr, string argstr,
                 list<dag> pattern, InstFormat format> : Instruction {
  field bits<32> Inst;
  // Recorded for readers of the .td; the encoding itself is in Inst.
  InstFormat Format = format;
  let Namespace = "MiniRV";
  let Size = 4;
  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = opcodestr # "\t" # argstr;
  let Pattern = pattern;
}
