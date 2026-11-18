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
// Chapter 01 deliberately encoded LD/ST in the I format (a full 16-bit
// displacement, no space/size qualifier).  Chapter 06 turns them into the M
// format below and rewrites the chapter 01/02 encoder tests with it.
//
// The M format has to fit a displacement *and* three qualifiers into the 16
// bits the I format spends on the immediate alone.  The layout used here is
//
//   [15:5] imm11   signed byte displacement
//   [4:3]  space   0 global, 1 shared, 2 local, 3 constant
//   [2:1]  size    0 b8, 1 b16, 2 b32
//   [0]    sign    0 zero-extend, 1 sign-extend (loads; zero for stores)
//
// which is 11+2+2+1 = 16 bits exactly.  The plan's draft put the qualifiers in
// funct[10:5], but funct already overlaps the immediate there; moving them to
// the low five bits keeps every field contiguous and buys one more bit of
// reach (11 bits, +-1024 bytes) than the arithmetic allows any other way.
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
def OPC_ADDI : GPUMinOpcode<0x06>;  // add immediate (chapter 07)
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

/// J-type: `op offset21`, a PC-relative call or jump.  The offset counts
/// instruction words (chapter 07), which gives +-1Mi instructions of reach and
/// keeps the low two bits free.
class GPUMinJ<bits<6> op, dag outs, dag ins, string asmstr,
              list<dag> pattern = []>
    : GPUMinInst<outs, ins, asmstr, pattern> {
  bits<21> off;
  let Inst{31-26} = op;
  let Inst{25-21} = 0;
  let Inst{20-0} = off;
}

/// Memory load in the M format: `ld.<space>.<size> rd, imm(base)`.
///
/// `imm` is an 11-bit signed byte displacement, so a load can address
/// base-1024 .. base+1023 without wasting a register on the offset.
class GPUMinLdM<bits<6> op, bits<2> space, bits<2> size, bit sign,
                string asmstr>
    : GPUMinInst<(outs GPR:$rd), (ins GPR:$base, simm11:$imm), asmstr, []> {
  bits<5> rd;
  bits<5> base;
  bits<11> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = base;
  let Inst{15-5} = imm;
  let Inst{4-3} = space;
  let Inst{2-1} = size;
  let Inst{0} = sign;
  let mayLoad = 1;
}

/// Memory store in the M format: `st.<space>.<size> rs2, imm(base)`.
/// The stored value rides in the rd slot, as in the chapter 01 I format; bit 0
/// is always zero because a store never extends anything.
class GPUMinStM<bits<6> op, bits<2> space, bits<2> size, string asmstr>
    : GPUMinInst<(outs), (ins GPR:$rs2, GPR:$base, simm11:$imm), asmstr, []> {
  bits<5> rs2;
  bits<5> base;
  bits<11> imm;
  let Inst{31-26} = op;
  let Inst{25-21} = rs2;
  let Inst{20-16} = base;
  let Inst{15-5} = imm;
  let Inst{4-3} = space;
  let Inst{2-1} = size;
  let Inst{0} = 0;
  let mayStore = 1;
}

/// Predicated select: `selp rd, pd, rs1, rs2` (rd = pd ? rs1 : rs2).
/// The predicate occupies bits [10:8] of what is otherwise the R2 funct
/// field; [7:0] stay zero.  Keeping the three register fields in their usual
/// slots means the encoder, the printer and the decoder all treat SELP as an
/// R2 with a 3-bit tail instead of inventing a sixth layout.
class GPUMinSelp<bits<6> op, string asmstr>
    : GPUMinInst<(outs GPR:$rd), (ins PRED:$pd, GPR:$rs1, GPR:$rs2), asmstr, []> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;
  bits<3> pd;
  let Inst{31-26} = op;
  let Inst{25-21} = rd;
  let Inst{20-16} = rs1;
  let Inst{15-11} = rs2;
  let Inst{10-8} = pd;
  let Inst{7-0} = 0;
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
