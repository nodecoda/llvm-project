//===-- GPUMinInstrFormats.td - GPUMin instruction formats --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// GM32: fixed 32-bit instructions, six bits of opcode, five bits of register
// number.  The five formats are (plan section 4.2):
//
//   R   31      26 25   21 20   16 15   11 10            0
//       opcode      rd      rs1     rs2       funct
//
//   I   31      26 25   21 20   16 15                  0
//       opcode      rd      rs1          imm16
//
//   M   31      26 25   21 20   16 15                  0
//       opcode    rd/rs2    base          imm16
//
//   B   31      26 25   21 20   16 15                  0
//       opcode     pred      rs1       offset16 (PC-rel)
//
//   J   31      26 25   21 20                          0
//       opcode     link            offset21 (PC-rel)
//
// Chapter 01 defines R, I and M and the instruction classes that use them.  B
// and J arrive with control flow (chapter 05) and calls (chapter 07).
//
// Deliberate simplification: chapter 01 encodes LD/ST in the I format, i.e.
// with a full 16-bit displacement and no space/size qualifier.  Chapter 06
// turns them into the M format with a funct field
// ([10:8] space, [7:6] size, [5] sign-extend), which shrinks the displacement
// to 11 bits; that is the first time the distinction is observable, and the
// chapter rewrites these two definitions together with the encoder tests.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Base classes
//===----------------------------------------------------------------------===//

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

class GPUMinPseudo<dag outs, dag ins, string asmstr, list<dag> pattern>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  let isPseudo = 1;
  let isCodeGenOnly = 1;
  let Size = 0;
}

//===----------------------------------------------------------------------===//
// Opcode map (plan section 4.3)
//===----------------------------------------------------------------------===//

class GPUMinOpcode<bits<6> val> {
  bits<6> Value = val;
}

def OPC_MOV  : GPUMinOpcode<0x01>;  // reg / imm / uniform / special
def OPC_ALU  : GPUMinOpcode<0x02>;  // add/sub/and/or/xor/shl/shr/sar
def OPC_SETP : GPUMinOpcode<0x03>;  // predicate compare
def OPC_SELP : GPUMinOpcode<0x04>;  // predicated select
def OPC_MOVI : GPUMinOpcode<0x05>;  // move immediate
def OPC_LD   : GPUMinOpcode<0x08>;
def OPC_ST   : GPUMinOpcode<0x09>;
def OPC_BRA  : GPUMinOpcode<0x10>;
def OPC_CALL : GPUMinOpcode<0x11>;
def OPC_SYS  : GPUMinOpcode<0x12>;  // ret / exit / bar / special

//===----------------------------------------------------------------------===//
// ALU function codes (opcode 0x02)
//===----------------------------------------------------------------------===//
//
// The low four bits select the operation; bit 10 marks the immediate form
// (ALUI), which arrives together with the ALU patterns in chapter 04.

//
//   add 0  sub 1  and 2  or 3  xor 4  shl 5  shr 6  sar 7

//===----------------------------------------------------------------------===//
// SETP condition codes (opcode 0x03)
//===----------------------------------------------------------------------===//
//
// func = { unsigned, 3-bit relation }: bit 3 selects unsigned comparison,
// bits 2:0 select eq/ne/lt/le/gt/ge.
//
//   eq 0  ne 1  lt 2  le 3  gt 4  ge 5

//===----------------------------------------------------------------------===//
// Instruction format classes
//===----------------------------------------------------------------------===//

/// R-type with two source registers: `op rd, rs1, rs2`.
class GPUMinR2<bits<6> op, bits<11> funct, dag outs, dag ins, string asmstr,
               list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = rs1;
  let Inst{15-11} = rs2;
  let Inst{10-0} = funct;
}

/// R-type with one source register: `op rd, rs1`.  rs2 is zero.
class GPUMinR1<bits<6> op, bits<11> funct, dag outs, dag ins, string asmstr,
               list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  bits<5> rs1;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = rs1;
  let Inst{15-11} = 0;
  let Inst{10-0} = funct;
}

/// I-type: `op rd, rs1, imm16`.
class GPUMinI<bits<6> op, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  bits<5> rs1;
  bits<16> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = rs1;
  let Inst{15-0} = imm;
}

/// I-type move-immediate form: rs1 is fixed to zero, so the whole 16-bit
/// field is a constant rather than an offset.
class GPUMinMovI<bits<6> op, dag outs, dag ins, string asmstr,
                 list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  bits<16> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = 0;
  let Inst{15-0} = imm;
}

/// Memory load: `ld rd, imm(rs1)`.
class GPUMinLd<bits<6> op, dag outs, dag ins, string asmstr,
               list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<5> rd;
  bits<5> rs1;
  bits<16> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = rs1;
  let Inst{15-0} = imm;
}

/// Memory store: `st rs2, imm(rs1)`.  The rd slot holds the stored value.
class GPUMinSt<bits<6> op, dag ins, string asmstr, list<dag> pattern = []>
    : GPUMinInst<(outs), ins, asmstr, pattern> {
  bits<5> rs2;
  bits<5> rs1;
  bits<16> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rs2;
  let Inst{20-16} = rs1;
  let Inst{15-0} = imm;
}

/// Predicate compare: `setp.lt pd, rs1, rs2`.
class GPUMinSetp<bits<6> op, bits<11> funct, string asmstr>
    : GPUMinInst<(outs PRED:$pd), (ins GPR:$rs1, GPR:$rs2), asmstr, []> {
  bits<5> pd;
  bits<5> rs1;
  bits<5> rs2;
  let Inst{31-26} = op;
  let Inst{25-21} = pd;
  let Inst{20-16} = rs1;
  let Inst{15-11} = rs2;
  let Inst{10-0} = funct;
}
