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
def BranchTargetAsmOperand : GPUMinImmAsmOperand<"BranchTarget", "isBranchTarget">;

def simm16 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm16AsmOperand;
  let DecoderMethod = "DecodeSImm16";
}

/// A branch target.  The assembly text is a label (or a word offset); the
/// encoder turns it into a PC-relative 16-bit displacement, which is why the
/// operand type is OtherVT: it never becomes a value in a register.
def brtarget16 : Operand<OtherVT> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = BranchTargetAsmOperand;
  let EncoderMethod = "getBranchTargetOpValue";
  let DecoderMethod = "DecodeBrTarget16";
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

def SELP : GPUMinSelp<OPC_SELP.Value, "selp $rd, $pd, $rs1, $rs2">;

// funct = { unsigned, relation[2:0] } (GPUMinInstrFormats.td).
def SETP_EQ  : GPUMinSetp<OPC_SETP.Value, 0x0, "setp.eq $pd, $rs1, $rs2">;
def SETP_NE  : GPUMinSetp<OPC_SETP.Value, 0x1, "setp.ne $pd, $rs1, $rs2">;
def SETP_LT  : GPUMinSetp<OPC_SETP.Value, 0x2, "setp.lt $pd, $rs1, $rs2">;
def SETP_LE  : GPUMinSetp<OPC_SETP.Value, 0x3, "setp.le $pd, $rs1, $rs2">;
def SETP_GT  : GPUMinSetp<OPC_SETP.Value, 0x4, "setp.gt $pd, $rs1, $rs2">;
def SETP_GE  : GPUMinSetp<OPC_SETP.Value, 0x5, "setp.ge $pd, $rs1, $rs2">;

// Bit 3 selects unsigned comparison.
def SETP_UEQ : GPUMinSetp<OPC_SETP.Value, 0x8, "setp.ueq $pd, $rs1, $rs2">;
def SETP_UNE : GPUMinSetp<OPC_SETP.Value, 0x9, "setp.une $pd, $rs1, $rs2">;
def SETP_ULT : GPUMinSetp<OPC_SETP.Value, 0xA, "setp.ult $pd, $rs1, $rs2">;
def SETP_ULE : GPUMinSetp<OPC_SETP.Value, 0xB, "setp.ule $pd, $rs1, $rs2">;
def SETP_UGT : GPUMinSetp<OPC_SETP.Value, 0xC, "setp.ugt $pd, $rs1, $rs2">;
def SETP_UGE : GPUMinSetp<OPC_SETP.Value, 0xD, "setp.uge $pd, $rs1, $rs2">;

//===----------------------------------------------------------------------===//
// SelectionDAG patterns (chapter 04)
//===----------------------------------------------------------------------===//

// Integer ALU.  The three shifts are three different DAG nodes: `shl`, the
// logical `srl`, and the arithmetic `sra`.  Mapping them to the right funct
// value is the whole point of the table in GPUMinInstrFormats.td.
def : Pat<(add  GPR:$rs1, GPR:$rs2), (ADD $rs1, $rs2)>;
def : Pat<(sub  GPR:$rs1, GPR:$rs2), (SUB $rs1, $rs2)>;
def : Pat<(and  GPR:$rs1, GPR:$rs2), (AND $rs1, $rs2)>;
def : Pat<(or   GPR:$rs1, GPR:$rs2), (OR  $rs1, $rs2)>;
def : Pat<(xor  GPR:$rs1, GPR:$rs2), (XOR $rs1, $rs2)>;
def : Pat<(shl  GPR:$rs1, GPR:$rs2), (SHL $rs1, $rs2)>;
def : Pat<(srl  GPR:$rs1, GPR:$rs2), (SHR $rs1, $rs2)>;
def : Pat<(sra  GPR:$rs1, GPR:$rs2), (SAR $rs1, $rs2)>;

// Comparisons produce a predicate register, which is why i1 has to be a legal
// type for this target (see GPUMinISelLowering.cpp).
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETEQ), (SETP_EQ $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETNE), (SETP_NE $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETLT), (SETP_LT $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETLE), (SETP_LE $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETGT), (SETP_GT $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETGE), (SETP_GE $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETUEQ), (SETP_UEQ $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETUNE), (SETP_UNE $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETULT), (SETP_ULT $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETULE), (SETP_ULE $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETUGT), (SETP_UGT $rs1, $rs2)>;
def : Pat<(setcc GPR:$rs1, GPR:$rs2, SETUGE), (SETP_UGE $rs1, $rs2)>;

// `select` on a predicate register.  The i1 condition is a real value in the
// PRED file, which is why chapter 04 makes i1 a legal type.
def : Pat<(select i1:$pd, GPR:$rs1, GPR:$rs2), (SELP $pd, $rs1, $rs2)>;

//===----------------------------------------------------------------------===//
// Memory (opcodes 0x08 / 0x09)
//===----------------------------------------------------------------------===//

def LD : GPUMinLd<OPC_LD.Value, (outs GPR:$rd), (ins GPR:$rs1, simm16:$imm),
                  "ld $rd, ${imm}(${rs1})", []>;

def ST : GPUMinSt<OPC_ST.Value, (ins GPR:$rs2, GPR:$rs1, simm16:$imm),
                  "st $rs2, ${imm}(${rs1})", []>;

//===----------------------------------------------------------------------===//
// Control flow (opcode 0x10)
//===----------------------------------------------------------------------===//
//
// B format: opcode[31:26] | hasPred[25] | pred[24:22] | 0[21:16] | off16[15:0]
//
// The offset counts instruction words, not bytes: every GM32 word is four
// bytes, so a word offset reaches +-32Ki instructions and can never be
// misaligned.  Bit 25 distinguishes `bra target` from `bra pN, target`;
// without it p0 and "always" would encode identically.

class GPUMinBra<bits<6> op, dag ins, string asmstr, list<dag> pattern = []>
    : GPUMinInst<(outs), ins, asmstr, pattern> {
  bits<16> off;
  let Inst{31-26} = op;
  let Inst{25-21} = 0;
  let Inst{20-16} = 0;
  let Inst{15-0} = off;
}

class GPUMinBraP<bits<6> op, string asmstr, list<dag> pattern = []>
    : GPUMinInst<(outs), (ins PRED:$pd, brtarget16:$off), asmstr, pattern> {
  bits<3> pd;
  bits<16> off;
  let Inst{31-26} = op;
  let Inst{25} = 1;
  let Inst{24-22} = pd;
  let Inst{21-16} = 0;
  let Inst{15-0} = off;
}

def BRA : GPUMinBra<OPC_BRA.Value, (ins brtarget16:$off), "bra $off", []> {
  let isBranch = 1;
  let isTerminator = 1;
  let isBarrier = 1;
}

def BRA_P : GPUMinBraP<OPC_BRA.Value, "bra $pd, $off"> {
  let isBranch = 1;
  let isTerminator = 1;
}

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

// Conditional branch: (chain, i1 predicate, target).  Lowered to BRA_P.
def SDT_GPUMinBrCond : SDTypeProfile<0, 2, [SDTCisVT<0, i1>]>;
def GPUMinBrCond : SDNode<"GPUMinISD::BRCOND", SDT_GPUMinBrCond,
                          [SDNPHasChain]>;

def : Pat<(br bb:$dst), (BRA brtarget16:$dst)>;
def : Pat<(GPUMinBrCond i1:$pd, bb:$dst), (BRA_P $pd, brtarget16:$dst)>;

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
