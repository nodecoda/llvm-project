//===-- GPUMinInstrInfo.td - GPUMin instruction definitions -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 00 defines only the return pseudo instruction.  It is enough to give
// TableGen a target instruction hierarchy and to let chapter 03 attach the
// first real pattern; the GM32 opcodes and formats arrive in chapter 01.
//
//===----------------------------------------------------------------------===//

// Every GM32 instruction is a fixed 32-bit word.  The `Inst` field is filled
// in by the per-format classes in chapter 01.
class GPUMinInst<dag outs, dag ins, string asmstr, list<dag> pattern>
    : Instruction {
  field bits<32> Inst;
  let Namespace = "GPUMin";
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
  let Size = 4;
}

// Pseudo instructions never reach the encoder: the AsmPrinter expands them
// into real GM32 words (PseudoInstExpansion in later chapters) or handles them
// directly.
class GPUMinPseudo<dag outs, dag ins, string asmstr, list<dag> pattern>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  let isPseudo = 1;
  let isCodeGenOnly = 1;
  let Size = 0;
}

//===----------------------------------------------------------------------===//
// Chapter 00's only real instruction.
//===----------------------------------------------------------------------===//
//
// GM32 has no operands to encode here: the whole word is opcode 0x12 with
// funct 0, which is exactly what makes EXIT a convenient first instruction for
// the MC layer.  Chapter 01 replaces the hand-written bit assignment with the
// shared R/I/M/B/J format classes.
def EXIT : GPUMinInst<(outs), (ins), "exit", []> {
  let Inst{31-26} = 0x12;  // opcode
  let Inst{10-0} = 0;      // funct = 0: exit / kernel return
}

//===----------------------------------------------------------------------===//
// Selection DAG nodes
//===----------------------------------------------------------------------===//

// Return from the current function.  Values have already been moved to the
// return registers by LowerReturn, so the node only carries chain and glue.
def GPUMinRet : SDNode<"GPUMinISD::RET", SDTNone,
                       [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

def PseudoRET : GPUMinPseudo<(outs), (ins), "", [(GPUMinRet)]>,
                PseudoInstExpansion<(EXIT)> {
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
}
