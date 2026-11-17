//===-- RVMinInstrFormats.td - RVMin instruction formats ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The six RV32I instruction formats, laid out exactly as in the RISC-V
// unprivileged ISA manual (chapter "RV32I Base Integer Instruction Set"):
//
//   R   31        25 24    20 19    15 14  12 11     7 6        0
//       funct7       rs2       rs1      funct3    rd       opcode
//
//   I   31            20 19    15 14  12 11     7 6        0
//       imm[11:0]        rs1      funct3    rd       opcode
//
//   S   31     25 24    20 19    15 14  12 11     7 6        0
//       imm[11:5]  rs2       rs1      funct3    imm[4:0]  opcode
//
//   B   31 30    25 24    20 19    15 14  12 11  8 7  6        0
//       imm[12|10:5] rs2      rs1      funct3 imm[4:1|11] opcode
//
//   U   31            12 11     7 6        0
//       imm[31:12]       rd       opcode
//
//   J   31 30        21 20 19        12 11     7 6        0
//       imm[20|10:1|11|19:12]   rd       opcode
//
// Note one deliberate difference from llvm/lib/Target/RISCV: branch and jump
// immediates are stored here as the *full byte offset* (13 and 21 bits, bit 0
// always zero) instead of a pre-shifted 12/20-bit value.  That keeps the
// lowering code in chapters 4-5 free of ">> 1" / "<< 1" conversions, and the
// operand classes reject offsets that are not 2-byte aligned.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Base classes
//===----------------------------------------------------------------------===//

// Instruction formats as recorded in TSFlags.  The MC code emitter (chapter 2)
// and the fixup machinery use this to know how an immediate must be patched.
class RVMinInstFormat<bits<5> val> {
  bits<5> Value = val;
}

def FmtPseudo : RVMinInstFormat<0>;
def FmtR      : RVMinInstFormat<1>;
def FmtI      : RVMinInstFormat<2>;
def FmtS      : RVMinInstFormat<3>;
def FmtB      : RVMinInstFormat<4>;
def FmtU      : RVMinInstFormat<5>;
def FmtJ      : RVMinInstFormat<6>;
def FmtR4     : RVMinInstFormat<7>;  // chapter 40: fused multiply-add
// Chapter 43: the eight 16-bit compressed formats plus the CA form used by
// c.sub/c.xor/c.or/c.and (which upstream models separately from CB).
def FmtCR     : RVMinInstFormat<8>;
def FmtCI     : RVMinInstFormat<9>;
def FmtCSS    : RVMinInstFormat<10>;
def FmtCIW    : RVMinInstFormat<11>;
def FmtCL     : RVMinInstFormat<12>;
def FmtCS     : RVMinInstFormat<13>;
def FmtCB     : RVMinInstFormat<14>;
def FmtCJ     : RVMinInstFormat<15>;
def FmtCA     : RVMinInstFormat<16>;

// Opcodes from the RISC-V base opcode map.  Only the ones RV32I needs.
class RVMinOpcode<bits<7> val> {
  bits<7> Value = val;
}

def OPC_LOAD    : RVMinOpcode<0b0000011>;
def OPC_IMM     : RVMinOpcode<0b0010011>;  // OP-IMM
def OPC_AUIPC   : RVMinOpcode<0b0010111>;
def OPC_STORE   : RVMinOpcode<0b0100011>;
def OPC_OP      : RVMinOpcode<0b0110011>;
def OPC_LUI     : RVMinOpcode<0b0110111>;
def OPC_BRANCH  : RVMinOpcode<0b1100011>;
def OPC_JALR    : RVMinOpcode<0b1100111>;
def OPC_JAL     : RVMinOpcode<0b1101111>;
def OPC_SYSTEM  : RVMinOpcode<0b1110011>;
def OPC_AMO     : RVMinOpcode<0b0101111>;  // ATOMIC
def OPC_MISC_MEM: RVMinOpcode<0b0001111>;  // FENCE / FENCE.I
// Chapter 40: floating-point opcodes.
def OPC_LOAD_FP : RVMinOpcode<0b0000111>;  // FLW / FLD
def OPC_STORE_FP: RVMinOpcode<0b0100111>;  // FSW / FSD
def OPC_MADD    : RVMinOpcode<0b1000011>;
def OPC_MSUB    : RVMinOpcode<0b1000111>;
def OPC_NMSUB   : RVMinOpcode<0b1001011>;
def OPC_NMADD   : RVMinOpcode<0b1001111>;
def OPC_OP_FP   : RVMinOpcode<0b1010011>;

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
  // Keep pseudos out of the assembler: they have no mnemonic and no encoding.
  let isCodeGenOnly = 1;
  let Size = 0;
  let TSFlags{4-0} = FmtPseudo.Value;
}

//===----------------------------------------------------------------------===//
// R-type: funct7 | rs2 | rs1 | funct3 | rd | opcode
//===----------------------------------------------------------------------===//

class RVMinRInst<bits<7> funct7, bits<3> funct3, RVMinOpcode opcode,
                 dag outs, dag ins, string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{31-25} = funct7;

  let TSFlags{4-0} = FmtR.Value;
}

//===----------------------------------------------------------------------===//
// I-type: imm[11:0] | rs1 | funct3 | rd | opcode
//===----------------------------------------------------------------------===//

class RVMinIInst<bits<3> funct3, RVMinOpcode opcode, dag outs, dag ins,
                 string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<5> rs1;
  bits<12> imm12;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{31-20} = imm12;

  let TSFlags{4-0} = FmtI.Value;
}

// I-type with a 5-bit shift amount in imm[24:20]; imm[31:25] is the funct7
// field (0b0000000 for logical shifts, 0b0100000 for the arithmetic one).
class RVMinIShiftInst<bits<7> funct7, bits<3> funct3, RVMinOpcode opcode,
                      dag outs, dag ins, string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> shamt;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = shamt;
  let Inst{31-25} = funct7;

  let TSFlags{4-0} = FmtI.Value;
}

//===----------------------------------------------------------------------===//
// S-type: imm[11:5] | rs2 | rs1 | funct3 | imm[4:0] | opcode
//===----------------------------------------------------------------------===//

class RVMinSInst<bits<3> funct3, RVMinOpcode opcode, dag outs, dag ins,
                 string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rs2;
  bits<5> rs1;
  bits<12> imm12;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = imm12{4-0};
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{31-25} = imm12{11-5};

  let TSFlags{4-0} = FmtS.Value;
}

//===----------------------------------------------------------------------===//
// B-type: imm[12|10:5] | rs2 | rs1 | funct3 | imm[4:1|11] | opcode
//===----------------------------------------------------------------------===//

class RVMinBInst<bits<3> funct3, RVMinOpcode opcode, dag outs, dag ins,
                 string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rs2;
  bits<5> rs1;
  bits<13> imm13;

  let Inst{6-0}   = opcode.Value;
  let Inst{7}     = imm13{11};
  let Inst{11-8}  = imm13{4-1};
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{30-25} = imm13{10-5};
  let Inst{31}    = imm13{12};

  let TSFlags{4-0} = FmtB.Value;
}

//===----------------------------------------------------------------------===//
// U-type: imm[31:12] | rd | opcode
//===----------------------------------------------------------------------===//

class RVMinUInst<RVMinOpcode opcode, dag outs, dag ins, string mnemonic,
                 string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<20> imm20;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{31-12} = imm20;

  let TSFlags{4-0} = FmtU.Value;
}

//===----------------------------------------------------------------------===//
// J-type: imm[20|10:1|11|19:12] | rd | opcode
//===----------------------------------------------------------------------===//

class RVMinJInst<RVMinOpcode opcode, dag outs, dag ins, string mnemonic,
                 string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<21> imm21;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{19-12} = imm21{19-12};
  let Inst{20}    = imm21{11};
  let Inst{30-21} = imm21{10-1};
  let Inst{31}    = imm21{20};

  let TSFlags{4-0} = FmtJ.Value;
}

//===----------------------------------------------------------------------===//
// R4-type: rs3 | fmt | rs2 | rs1 | rm | rd | opcode
//===----------------------------------------------------------------------===//
//
// Used by the fused multiply-add instructions.  `fmt` is 00 for single
// precision and 01 for double precision; `rm` is the rounding mode (111 for
// the dynamic mode, which is what every instruction in this course uses).

class RVMinR4Inst<bits<2> fmt, RVMinOpcode opcode, dag outs, dag ins,
                  string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;
  bits<5> rs3;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = 0b111;   // dynamic rounding mode
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{26-25} = fmt;
  let Inst{31-27} = rs3;

  let TSFlags{4-0} = FmtR4.Value;
}

//===----------------------------------------------------------------------===//
// R-type with a fixed rs2 field
//===----------------------------------------------------------------------===//
//
// OP-FP encodes unary operations (fsqrt, fcvt, fmv, fclass) with rs2 as a
// sub-opcode instead of a register.  The bit layout is otherwise the R-type
// layout, so the MC encoder needs no special case.

class RVMinRInstFixedRS2<bits<7> funct7, bits<3> funct3, bits<5> rs2val,
                         RVMinOpcode opcode, dag outs, dag ins,
                         string mnemonic, string argstr>
    : RVMinInst<outs, ins, mnemonic # " " # argstr, []> {
  bits<5> rd;
  bits<5> rs1;

  let Inst{6-0}   = opcode.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2val;
  let Inst{31-25} = funct7;

  let TSFlags{4-0} = FmtR.Value;
}
