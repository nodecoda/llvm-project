//===-- RVMinInstrInfo.td - RVMin instruction definitions ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 1: the core RV32I instruction set.
//
// Every instruction here is a real, encodable RV32I instruction (no aliases,
// no pseudo-instructions yet).  The list is the subset the rest of the tutorial
// selects from:
//
//   ALU register-register : ADD SUB AND OR XOR SLL SRL SRA SLT SLTU
//   ALU immediate         : ADDI ANDI ORI XORI SLTI SLTIU SLLI SRLI SRAI
//   upper immediates      : LUI AUIPC
//   control flow          : JAL JALR BEQ BNE BLT BGE
//   memory                : LW SW  (byte/half-word in chapter 6)
//
// The immediate operands reject values that cannot be encoded, so a lowering
// bug shows up as a verifier/isel failure instead of silently truncated code.
//
//===----------------------------------------------------------------------===//

include "RVMinInstrFormats.td"

//===----------------------------------------------------------------------===//
// Immediate operands
//===----------------------------------------------------------------------===//

// Match classes for the assembler.  The generated asm matcher calls these
// predicates on RVMinOperand to decide which instruction form matches a parsed
// immediate.  Keeping the classes distinct (rather than using a single "Imm"
// class) lets the matcher reject e.g. `slli a0, a0, 32`.
class RVMinImmAsmOperand<string name, string predicate> : AsmOperandClass {
  let Name = name;
  let PredicateMethod = predicate;
  let RenderMethod = "addImmOperands";
}

def SImm12AsmOperand     : RVMinImmAsmOperand<"SImm12", "isSImm12">;
def UImm5AsmOperand      : RVMinImmAsmOperand<"UImm5", "isUImm5">;
def UImm20AsmOperand     : RVMinImmAsmOperand<"UImm20", "isUImm20">;
def SImm13Lsb0AsmOperand : RVMinImmAsmOperand<"SImm13Lsb0", "isSImm13Lsb0">;
def SImm21Lsb0AsmOperand : RVMinImmAsmOperand<"SImm21Lsb0", "isSImm21Lsb0">;

// 12-bit signed immediate: ADDI/ANDI/ORI/XORI/SLTI/SLTIU, LW/SW offsets,
// JALR offsets.  Encoded as-is in imm[11:0].
def simm12 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm12AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isInt<12>(Imm);
  }];
}

// 5-bit unsigned immediate: shift amounts.  RV32I encodes these in imm[24:20].
def uimm5 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = UImm5AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isUInt<5>(Imm);
  }];
}

// 20-bit unsigned immediate: LUI/AUIPC's imm[31:12] field.
def uimm20 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = UImm20AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isUInt<20>(Imm);
  }];
}

// 13-bit signed byte offset with bit 0 clear (branch target).
def simm13_lsb0 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm13Lsb0AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isShiftedInt<12, 1>(Imm);
  }];
}

// 21-bit signed byte offset with bit 0 clear (JAL target).
def simm21_lsb0 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm21Lsb0AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isShiftedInt<20, 1>(Imm);
  }];
}

//===----------------------------------------------------------------------===//
// ALU: register-register (R-type)
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def ADD  : RVMinRInst<0b0000000, 0b000, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "add", "$rd, $rs1, $rs2">;
def SUB  : RVMinRInst<0b0100000, 0b000, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "sub", "$rd, $rs1, $rs2">;
def SLT  : RVMinRInst<0b0000000, 0b010, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "slt", "$rd, $rs1, $rs2">;
def SLTU : RVMinRInst<0b0000000, 0b011, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "sltu", "$rd, $rs1, $rs2">;
def XOR  : RVMinRInst<0b0000000, 0b100, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "xor", "$rd, $rs1, $rs2">;
def SLL  : RVMinRInst<0b0000000, 0b001, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "sll", "$rd, $rs1, $rs2">;
def SRL  : RVMinRInst<0b0000000, 0b101, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "srl", "$rd, $rs1, $rs2">;
def SRA  : RVMinRInst<0b0100000, 0b101, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "sra", "$rd, $rs1, $rs2">;
def OR   : RVMinRInst<0b0000000, 0b110, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "or", "$rd, $rs1, $rs2">;
def AND  : RVMinRInst<0b0000000, 0b111, OPC_OP, (outs GPR:$rd),
                     (ins GPR:$rs1, GPR:$rs2), "and", "$rd, $rs1, $rs2">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0

//===----------------------------------------------------------------------===//
// ALU: immediate (I-type)
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def ADDI  : RVMinIInst<0b000, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, simm12:$imm12), "addi",
                      "$rd, $rs1, $imm12">;
def SLTI  : RVMinIInst<0b010, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, simm12:$imm12), "slti",
                      "$rd, $rs1, $imm12">;
def SLTIU : RVMinIInst<0b011, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, simm12:$imm12), "sltiu",
                      "$rd, $rs1, $imm12">;
def XORI  : RVMinIInst<0b100, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, simm12:$imm12), "xori",
                      "$rd, $rs1, $imm12">;
def ORI   : RVMinIInst<0b110, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, simm12:$imm12), "ori",
                      "$rd, $rs1, $imm12">;
def ANDI  : RVMinIInst<0b111, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, simm12:$imm12), "andi",
                      "$rd, $rs1, $imm12">;

def SLLI  : RVMinIShiftInst<0b0000000, 0b001, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, uimm5:$shamt), "slli",
                      "$rd, $rs1, $shamt">;
def SRLI  : RVMinIShiftInst<0b0000000, 0b101, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, uimm5:$shamt), "srli",
                      "$rd, $rs1, $shamt">;
def SRAI  : RVMinIShiftInst<0b0100000, 0b101, OPC_IMM, (outs GPR:$rd),
                      (ins GPR:$rs1, uimm5:$shamt), "srai",
                      "$rd, $rs1, $shamt">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0

//===----------------------------------------------------------------------===//
// Upper immediates (U-type)
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def LUI   : RVMinUInst<OPC_LUI, (outs GPR:$rd), (ins uimm20:$imm20), "lui",
                      "$rd, $imm20">;
def AUIPC : RVMinUInst<OPC_AUIPC, (outs GPR:$rd), (ins uimm20:$imm20), "auipc",
                      "$rd, $imm20">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

let isTerminator = 1, isBarrier = 1, hasSideEffects = 0, mayLoad = 0,
    mayStore = 0 in {
def JAL  : RVMinJInst<OPC_JAL, (outs GPR:$rd), (ins simm21_lsb0:$imm21), "jal",
                     "$rd, $imm21">;
def JALR : RVMinIInst<0b000, OPC_JALR, (outs GPR:$rd),
                     (ins GPR:$rs1, simm12:$imm12), "jalr",
                     "$rd, ${imm12}(${rs1})">;
} // isTerminator = 1, isBarrier = 1

let isBranch = 1, isTerminator = 1, isBarrier = 1, hasSideEffects = 0,
    mayLoad = 0, mayStore = 0 in {
def BEQ : RVMinBInst<0b000, OPC_BRANCH, (outs),
                    (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm13), "beq",
                    "$rs1, $rs2, $imm13">;
def BNE : RVMinBInst<0b001, OPC_BRANCH, (outs),
                    (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm13), "bne",
                    "$rs1, $rs2, $imm13">;
def BLT : RVMinBInst<0b100, OPC_BRANCH, (outs),
                    (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm13), "blt",
                    "$rs1, $rs2, $imm13">;
def BGE : RVMinBInst<0b101, OPC_BRANCH, (outs),
                    (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm13), "bge",
                    "$rs1, $rs2, $imm13">;
} // isBranch = 1, isTerminator = 1, isBarrier = 1

//===----------------------------------------------------------------------===//
// Memory (chapter 6 adds the narrower accesses)
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 1, mayStore = 0 in
def LW : RVMinIInst<0b010, OPC_LOAD, (outs GPR:$rd),
                   (ins GPR:$rs1, simm12:$imm12), "lw",
                   "$rd, ${imm12}(${rs1})">;

let hasSideEffects = 1, mayLoad = 0, mayStore = 1 in
def SW : RVMinSInst<0b010, OPC_STORE, (outs),
                   (ins GPR:$rs2, GPR:$rs1, simm12:$imm12), "sw",
                   "$rs2, ${imm12}(${rs1})">;

//===----------------------------------------------------------------------===//
// Pseudo instructions (no encoding of their own)
//===----------------------------------------------------------------------===//

def ADJCALLSTACKDOWN : RVMinPseudo<(outs), (ins i32imm:$amt), "", []>;
def ADJCALLSTACKUP : RVMinPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2), "", []>;
