//===-- GPUMinInstrInfo.td - GPUMin instruction definitions -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 00 introduced the instruction hierarchy and one real instruction
// (EXIT).  Chapter 01 adds the operand classes and the first GM32 instructions:
//
//   MOV/MOVI                       data movement
//   ADD/SUB/AND/OR/XOR/SHL/SHR/SAR integer ALU
//   SETP.LT/SETP.EQ                predicate compare
//   LD/ST                          global/shared/local load and store
//   EXIT                           kernel return
//
// None of them is selected yet: patterns and lowering arrive in chapters 03-06.
// What this chapter proves is that the *description* is accepted by TableGen —
// that the operand classes, the format classes and the opcode map agree.
//
//===----------------------------------------------------------------------===//

include "GPUMinInstrFormats.td"

//===----------------------------------------------------------------------===//
// Operand classes
//===----------------------------------------------------------------------===//

/// A signed 16-bit immediate: the I-type displacement, the MOVI constant and,
/// from chapter 06, the load/store displacement.
class GPUMinImmAsmOperand<string name, string predicate> : AsmOperandClass {
  let Name = name;
  let PredicateMethod = predicate;
  let RenderMethod = "addImmOperands";
}

def SImm16AsmOperand : GPUMinImmAsmOperand<"SImm16", "isSImm16">;

def simm16 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm16AsmOperand;
  let DecoderMethod = "DecodeSImm16";
}

//===----------------------------------------------------------------------===//
// Data movement (opcode 0x01)
//===----------------------------------------------------------------------===//

def MOV : GPUMinR1<OPC_MOV.Value, 0, (outs GPR:$rd), (ins GPR:$rs1),
                   "mov $rd, $rs1", []>;

// The plan's opcode draft folds the immediate form into MOV (0x01).  Chapter
// 01 gives it its own opcode instead, because the decoder needs one decision
// bit to tell `mov rd, rs1` from `movi rd, imm` and every operand field
// ([25:21] and [15:0]) is already carrying data.  Chapter 02 revisits the
// encoding as a whole when the byte-exact tests arrive.
def MOVI : GPUMinMovI<OPC_MOVI.Value, (outs GPR:$rd), (ins simm16:$imm),
                      "movi $rd, $imm", []>;

//===----------------------------------------------------------------------===//
// Integer ALU (opcode 0x02)
//===----------------------------------------------------------------------===//

// funct[3:0] = 0 add, 1 sub, 2 and, 3 or, 4 xor, 5 shl, 6 shr, 7 sar
// (GPUMinInstrFormats.td).  funct[10:4] stays zero for the register forms.
multiclass GPUMinALU<bits<11> funct, string mnemonic> {
  def "" : GPUMinR2<OPC_ALU.Value, funct, (outs GPR:$rd),
                    (ins GPR:$rs1, GPR:$rs2), mnemonic#" $rd, $rs1, $rs2", []>;
}

defm ADD : GPUMinALU<0x0, "add">;
defm SUB : GPUMinALU<0x1, "sub">;
defm AND : GPUMinALU<0x2, "and">;
defm OR  : GPUMinALU<0x3, "or">;
defm XOR : GPUMinALU<0x4, "xor">;
defm SHL : GPUMinALU<0x5, "shl">;
defm SHR : GPUMinALU<0x6, "shr">;
defm SAR : GPUMinALU<0x7, "sar">;

//===----------------------------------------------------------------------===//
// Predicate compare (opcode 0x03)
//===----------------------------------------------------------------------===//
//
// The relation lives in funct[2:0]; funct[3] is the unsigned flag.  Only the
// signed forms are described in chapter 01 — the unsigned and the
// le/gt/ge relations arrive with the SETP lowering in chapter 04.

def SETP_LT : GPUMinSetp<OPC_SETP.Value, 0x2, "setp.lt $pd, $rs1, $rs2">;
def SETP_EQ : GPUMinSetp<OPC_SETP.Value, 0x0, "setp.eq $pd, $rs1, $rs2">;

//===----------------------------------------------------------------------===//
// Memory (opcodes 0x08 / 0x09)
//===----------------------------------------------------------------------===//

def LD : GPUMinLd<OPC_LD.Value, (outs GPR:$rd), (ins GPR:$rs1, simm16:$imm),
                  "ld $rd, ${imm}(${rs1})", []>;

def ST : GPUMinSt<OPC_ST.Value, (ins GPR:$rs2, GPR:$rs1, simm16:$imm),
                  "st $rs2, ${imm}(${rs1})", []>;

//===----------------------------------------------------------------------===//
// System (opcode 0x12)
//===----------------------------------------------------------------------===//

// EXIT has no operands: the whole word is opcode 0x12 with funct 0.  It is the
// kernel-visible "return": a kernel ends by exiting, and chapter 00 already
// lowers `ret void` to it through PseudoRET.
def EXIT : GPUMinInst<(outs), (ins), "exit", []> {
  let Inst{31-26} = OPC_SYS.Value;
  let Inst{10-0} = 0;
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
