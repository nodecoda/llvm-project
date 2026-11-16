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
def simm12 : Operand<i32>, ImmLeaf<i32, [{ return isInt<12>(Imm); }]> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm12AsmOperand;
  let DecoderMethod = "decodeSImmOperand<12>";
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isInt<12>(Imm);
  }];
}

// 5-bit unsigned immediate: shift amounts.  RV32I encodes these in imm[24:20].
def uimm5 : Operand<i32>, ImmLeaf<i32, [{ return isUInt<5>(Imm); }]> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = UImm5AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isUInt<5>(Imm);
  }];
}

// 20-bit immediate: LUI/AUIPC's imm[31:12] field.  The DAG carries constants
// as sign-extended i32, so the machine operand may be negative even though the
// encoded field is unsigned; the printer therefore always prints the low 20
// bits (`lui a0, 1048575` rather than `lui a0, -1`), which is also what the
// parser accepts.
def uimm20 : Operand<i32> {
  let PrintMethod = "printUImm20Operand";
  let ParserMatchClass = UImm20AsmOperand;
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && (isUInt<20>(Imm) || isInt<20>(Imm));
  }];
}

// 13-bit signed byte offset with bit 0 clear (branch target).  The operand
// type is OtherVT, not i32, because instruction-selection patterns match it
// against a basic block (`bb:$dst`).
def simm13_lsb0 : Operand<OtherVT> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm13Lsb0AsmOperand;
  let DecoderMethod = "decodeSImmBranchOperand<13>";
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isShiftedInt<12, 1>(Imm);
    return MCOp.isBareSymbolRef();
  }];
}

// 21-bit signed byte offset with bit 0 clear (JAL target).
def simm21_lsb0 : Operand<OtherVT> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm21Lsb0AsmOperand;
  let DecoderMethod = "decodeSImmBranchOperand<21>";
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isShiftedInt<20, 1>(Imm);
    return MCOp.isBareSymbolRef();
  }];
}

// A call target: a function symbol.  The DAG node carries a pointer-sized
// address (i32 on RV32), so the operand is typed i32; the pseudo itself is
// code-gen only and never reaches the MC layer.
def bare_symbol : Operand<i32> {
  let PrintMethod = "printOperand";
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

// Conditional branches may fall through to the next block, so they must not be
// marked isBarrier (that would tell the verifier the block has no fallthrough).
let isBranch = 1, isTerminator = 1, hasSideEffects = 0,
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
def BLTU : RVMinBInst<0b110, OPC_BRANCH, (outs),
                     (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm13), "bltu",
                     "$rs1, $rs2, $imm13">;
def BGEU : RVMinBInst<0b111, OPC_BRANCH, (outs),
                     (ins GPR:$rs1, GPR:$rs2, simm13_lsb0:$imm13), "bgeu",
                     "$rs1, $rs2, $imm13">;
} // isBranch = 1, isTerminator = 1, isBarrier = 1

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

// Loads: funct3 selects the width and the extension:
//   0b000 lb (sign-extend 8), 0b001 lh (16), 0b010 lw (32),
//   0b100 lbu (zero-extend 8), 0b101 lhu (zero-extend 16).
let hasSideEffects = 0, mayLoad = 1, mayStore = 0 in {
def LB  : RVMinIInst<0b000, OPC_LOAD, (outs GPR:$rd),
                    (ins GPR:$rs1, simm12:$imm12), "lb",
                    "$rd, ${imm12}(${rs1})">;
def LH  : RVMinIInst<0b001, OPC_LOAD, (outs GPR:$rd),
                    (ins GPR:$rs1, simm12:$imm12), "lh",
                    "$rd, ${imm12}(${rs1})">;
def LW  : RVMinIInst<0b010, OPC_LOAD, (outs GPR:$rd),
                    (ins GPR:$rs1, simm12:$imm12), "lw",
                    "$rd, ${imm12}(${rs1})">;
def LBU : RVMinIInst<0b100, OPC_LOAD, (outs GPR:$rd),
                    (ins GPR:$rs1, simm12:$imm12), "lbu",
                    "$rd, ${imm12}(${rs1})">;
def LHU : RVMinIInst<0b101, OPC_LOAD, (outs GPR:$rd),
                    (ins GPR:$rs1, simm12:$imm12), "lhu",
                    "$rd, ${imm12}(${rs1})">;
} // hasSideEffects = 0, mayLoad = 1, mayStore = 0

// Stores: funct3 0b000 sb, 0b001 sh, 0b010 sw.
let hasSideEffects = 1, mayLoad = 0, mayStore = 1 in {
def SB : RVMinSInst<0b000, OPC_STORE, (outs),
                   (ins GPR:$rs2, GPR:$rs1, simm12:$imm12), "sb",
                   "$rs2, ${imm12}(${rs1})">;
def SH : RVMinSInst<0b001, OPC_STORE, (outs),
                   (ins GPR:$rs2, GPR:$rs1, simm12:$imm12), "sh",
                   "$rs2, ${imm12}(${rs1})">;
def SW : RVMinSInst<0b010, OPC_STORE, (outs),
                   (ins GPR:$rs2, GPR:$rs1, simm12:$imm12), "sw",
                   "$rs2, ${imm12}(${rs1})">;
} // hasSideEffects = 1, mayLoad = 0, mayStore = 1

//===----------------------------------------------------------------------===//
// Selection DAG nodes
//===----------------------------------------------------------------------===//

// Return from the current function.  Register values have already been moved
// into a0/a1 by LowerReturn, so the node only carries chain and glue.
def RVMinRet : SDNode<"RVMinISD::RET_FLAG", SDTNone,
                      [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

// Conditional branch with a condition code: operands are
// (lhs, rhs, condcode, dest).  BR_CC nodes are produced by lowerBRCOND /
// lowerBR_CC in RVMinISelLowering.cpp.
def SDT_RVMinBrCC : SDTypeProfile<0, 4, [SDTCisSameAs<0, 1>,
                                         SDTCisVT<2, OtherVT>,
                                         SDTCisVT<3, OtherVT>]>;
def RVMinBrcc : SDNode<"RVMinISD::BR_CC", SDT_RVMinBrCC, [SDNPHasChain]>;

// Call node: (chain, callee, argument registers..., regmask, glue?).
//
// The callee is either a target global address / external symbol (direct call)
// or a plain value (indirect call).  The trailing operands are variadic: the
// pattern matcher copies the argument registers and the call-preserved mask
// onto the selected machine instruction, where the register allocator and the
// machine verifier can see them.
// Target-independent call-frame nodes, with the operand types RVMin uses.
def SDT_RVMinCallSeqStart : SDCallSeqStart<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;
def SDT_RVMinCallSeqEnd : SDCallSeqEnd<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;
def callseq_start : SDNode<"ISD::CALLSEQ_START", SDT_RVMinCallSeqStart,
                           [SDNPHasChain, SDNPOutGlue]>;
def callseq_end : SDNode<"ISD::CALLSEQ_END", SDT_RVMinCallSeqEnd,
                         [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;

def SDT_RVMinCall : SDTypeProfile<0, -1, [SDTCisPtrTy<0>]>;
def RVMinCall : SDNode<"RVMinISD::CALL", SDT_RVMinCall,
                       [SDNPHasChain, SDNPOutGlue, SDNPOptInGlue,
                        SDNPVariadic]>;

// Tail call (chapter 17): same operands as RVMinCall, but nothing continues
// afterwards -- the callee returns straight to our caller.  There is no glue
// result because the frame is already gone when this node executes.
def RVMinTail : SDNode<"RVMinISD::TAIL", SDT_RVMinCall,
                       [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

//===----------------------------------------------------------------------===//
// Selection patterns
//===----------------------------------------------------------------------===//

// Chapter 3: integer add.  Chapter 4 fills in the rest of the ALU and the
// comparisons; chapter 5 adds control flow.

// Arithmetic.  Constants are materialized by RVMinDAGToDAGISel::Select (ADDI
// for 12-bit values, LUI+ADDI otherwise) before these patterns are tried.
def : Pat<(add GPR:$rs1, GPR:$rs2), (ADD GPR:$rs1, GPR:$rs2)>;
def : Pat<(add GPR:$rs1, simm12:$imm), (ADDI GPR:$rs1, simm12:$imm)>;
def : Pat<(sub GPR:$rs1, GPR:$rs2), (SUB GPR:$rs1, GPR:$rs2)>;

// Logic.
def : Pat<(and GPR:$rs1, GPR:$rs2), (AND GPR:$rs1, GPR:$rs2)>;
def : Pat<(and GPR:$rs1, simm12:$imm), (ANDI GPR:$rs1, simm12:$imm)>;
def : Pat<(or GPR:$rs1, GPR:$rs2), (OR GPR:$rs1, GPR:$rs2)>;
def : Pat<(or GPR:$rs1, simm12:$imm), (ORI GPR:$rs1, simm12:$imm)>;
def : Pat<(xor GPR:$rs1, GPR:$rs2), (XOR GPR:$rs1, GPR:$rs2)>;
def : Pat<(xor GPR:$rs1, simm12:$imm), (XORI GPR:$rs1, simm12:$imm)>;

// Shifts.  RV32I only encodes 5-bit shift amounts, and LLVM shifts by >= 32
// are poison, so a plain SLL/SRL/SRA is the correct instruction.
def : Pat<(shl GPR:$rs1, GPR:$rs2), (SLL GPR:$rs1, GPR:$rs2)>;
def : Pat<(shl GPR:$rs1, uimm5:$shamt), (SLLI GPR:$rs1, uimm5:$shamt)>;
def : Pat<(srl GPR:$rs1, GPR:$rs2), (SRL GPR:$rs1, GPR:$rs2)>;
def : Pat<(srl GPR:$rs1, uimm5:$shamt), (SRLI GPR:$rs1, uimm5:$shamt)>;
def : Pat<(sra GPR:$rs1, GPR:$rs2), (SRA GPR:$rs1, GPR:$rs2)>;
def : Pat<(sra GPR:$rs1, uimm5:$shamt), (SRAI GPR:$rs1, uimm5:$shamt)>;

// Comparisons.  SLT/SLTU already produce 0/1, so the signed/unsigned ordered
// relations map directly; the non-strict ones invert one result bit with XORI.
def : Pat<(i32 (setlt (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLT GPR:$rs1, GPR:$rs2)>;
def : Pat<(i32 (setlt (i32 GPR:$rs1), simm12:$imm)),
          (SLTI GPR:$rs1, simm12:$imm)>;
def : Pat<(i32 (setgt (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLT GPR:$rs2, GPR:$rs1)>;
def : Pat<(i32 (setge (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (XORI (SLT GPR:$rs1, GPR:$rs2), 1)>;
def : Pat<(i32 (setle (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (XORI (SLT GPR:$rs2, GPR:$rs1), 1)>;

def : Pat<(i32 (setult (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLTU GPR:$rs1, GPR:$rs2)>;
def : Pat<(i32 (setult (i32 GPR:$rs1), simm12:$imm)),
          (SLTIU GPR:$rs1, simm12:$imm)>;
def : Pat<(i32 (setugt (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLTU GPR:$rs2, GPR:$rs1)>;
def : Pat<(i32 (setuge (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (XORI (SLTU GPR:$rs1, GPR:$rs2), 1)>;
def : Pat<(i32 (setule (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (XORI (SLTU GPR:$rs2, GPR:$rs1), 1)>;

// Equality goes through XOR: (a ^ b) == 0 is exactly a == b.
def : Pat<(i32 (seteq (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLTIU (XOR GPR:$rs1, GPR:$rs2), 1)>;
def : Pat<(i32 (seteq (i32 GPR:$rs1), 0)), (SLTIU GPR:$rs1, 1)>;
def : Pat<(i32 (setne (i32 GPR:$rs1), (i32 GPR:$rs2))),
          (SLTU X0, (XOR GPR:$rs1, GPR:$rs2))>;
def : Pat<(i32 (setne (i32 GPR:$rs1), 0)), (SLTU X0, GPR:$rs1)>;

// Conditional branches.  The DAG-level condition codes are canonicalized in C++
// (see lowerBR_CC), so only these six remain here.
multiclass RVMinBccPat<CondCode Cond, RVMinBInst Inst> {
  def : Pat<(RVMinBrcc GPR:$rs1, GPR:$rs2, Cond, bb:$dst),
            (Inst GPR:$rs1, GPR:$rs2, simm13_lsb0:$dst)>;
  // Compare against zero uses x0 instead of materializing a constant.
  def : Pat<(RVMinBrcc GPR:$rs1, 0, Cond, bb:$dst),
            (Inst GPR:$rs1, X0, simm13_lsb0:$dst)>;
}
defm : RVMinBccPat<SETEQ, BEQ>;
defm : RVMinBccPat<SETNE, BNE>;
defm : RVMinBccPat<SETLT, BLT>;
defm : RVMinBccPat<SETGE, BGE>;
defm : RVMinBccPat<SETULT, BLTU>;
defm : RVMinBccPat<SETUGE, BGEU>;

// Generic memory accesses.  Chapter 6 only legalizes i32; i8/i16 arrive with
// the type legalizer in chapter 9.  Frame-index addresses are folded into the
// immediate by RVMinDAGToDAGISel::Select, so these patterns only see values
// that are already in a register.
def : Pat<(i32 (load GPR:$addr)), (LW GPR:$addr, 0)>;
def : Pat<(i32 (load (add GPR:$addr, simm12:$off))),
          (LW GPR:$addr, simm12:$off)>;
def : Pat<(store GPR:$val, GPR:$addr), (SW GPR:$val, GPR:$addr, 0)>;
def : Pat<(store GPR:$val, (add GPR:$addr, simm12:$off)),
          (SW GPR:$val, GPR:$addr, simm12:$off)>;

// Chapter 9: narrow loads and truncating stores.
//
// The type legalizer promotes i8/i16 values to i32, so a `char`/`short` load
// arrives as an i32 load whose memory type is still i8/i16 and whose
// extension is part of the node:
//
//   sextloadi8  -> LB      zextloadi8  -> LBU
//   sextloadi16 -> LH      zextloadi16 -> LHU
//
// `extload` means "any extension is fine", so a zero-extending load is a
// valid lowering for it.  Stores go the other way: `truncstorei8/i16` write
// the low byte/halfword of an i32 register.
def : Pat<(i32 (sextloadi8 GPR:$addr)), (LB GPR:$addr, 0)>;
def : Pat<(i32 (zextloadi8 GPR:$addr)), (LBU GPR:$addr, 0)>;
def : Pat<(i32 (extloadi8 GPR:$addr)), (LBU GPR:$addr, 0)>;
def : Pat<(i32 (sextloadi8 (add GPR:$addr, simm12:$off))),
          (LB GPR:$addr, simm12:$off)>;
def : Pat<(i32 (zextloadi8 (add GPR:$addr, simm12:$off))),
          (LBU GPR:$addr, simm12:$off)>;
def : Pat<(i32 (extloadi8 (add GPR:$addr, simm12:$off))),
          (LBU GPR:$addr, simm12:$off)>;

def : Pat<(i32 (sextloadi16 GPR:$addr)), (LH GPR:$addr, 0)>;
def : Pat<(i32 (zextloadi16 GPR:$addr)), (LHU GPR:$addr, 0)>;
def : Pat<(i32 (extloadi16 GPR:$addr)), (LHU GPR:$addr, 0)>;
def : Pat<(i32 (sextloadi16 (add GPR:$addr, simm12:$off))),
          (LH GPR:$addr, simm12:$off)>;
def : Pat<(i32 (zextloadi16 (add GPR:$addr, simm12:$off))),
          (LHU GPR:$addr, simm12:$off)>;
def : Pat<(i32 (extloadi16 (add GPR:$addr, simm12:$off))),
          (LHU GPR:$addr, simm12:$off)>;

def : Pat<(truncstorei8 GPR:$val, GPR:$addr),
          (SB GPR:$val, GPR:$addr, 0)>;
def : Pat<(truncstorei8 GPR:$val, (add GPR:$addr, simm12:$off)),
          (SB GPR:$val, GPR:$addr, simm12:$off)>;
def : Pat<(truncstorei16 GPR:$val, GPR:$addr),
          (SH GPR:$val, GPR:$addr, 0)>;
def : Pat<(truncstorei16 GPR:$val, (add GPR:$addr, simm12:$off)),
          (SH GPR:$val, GPR:$addr, simm12:$off)>;


//===----------------------------------------------------------------------===//
// M extension (chapter 13)
//===----------------------------------------------------------------------===//
//
// Eight R-type instructions with funct7 = 0b0000001.  They are only available
// with `-mattr=+m` (or `-mcpu=generic-rv32-m`): the Predicates clause keeps
// both the instruction selector and the assembler honest.

let Predicates = [HasStdExtM] in {

def MUL    : RVMinRInst<0b0000001, 0b000, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "mul", "$rd, $rs1, $rs2">;
def MULH   : RVMinRInst<0b0000001, 0b001, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "mulh", "$rd, $rs1, $rs2">;
def MULHSU : RVMinRInst<0b0000001, 0b010, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "mulhsu", "$rd, $rs1, $rs2">;
def MULHU  : RVMinRInst<0b0000001, 0b011, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "mulhu", "$rd, $rs1, $rs2">;
def DIV    : RVMinRInst<0b0000001, 0b100, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "div", "$rd, $rs1, $rs2">;
def DIVU   : RVMinRInst<0b0000001, 0b101, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "divu", "$rd, $rs1, $rs2">;
def REM    : RVMinRInst<0b0000001, 0b110, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "rem", "$rd, $rs1, $rs2">;
def REMU   : RVMinRInst<0b0000001, 0b111, OPC_OP, (outs GPR:$rd),
                       (ins GPR:$rs1, GPR:$rs2), "remu", "$rd, $rs1, $rs2">;

def : Pat<(mul    GPR:$rs1, GPR:$rs2), (MUL    GPR:$rs1, GPR:$rs2)>;
def : Pat<(sdiv   GPR:$rs1, GPR:$rs2), (DIV    GPR:$rs1, GPR:$rs2)>;
def : Pat<(udiv   GPR:$rs1, GPR:$rs2), (DIVU   GPR:$rs1, GPR:$rs2)>;
def : Pat<(srem   GPR:$rs1, GPR:$rs2), (REM    GPR:$rs1, GPR:$rs2)>;
def : Pat<(urem   GPR:$rs1, GPR:$rs2), (REMU   GPR:$rs1, GPR:$rs2)>;
def : Pat<(mulhs  GPR:$rs1, GPR:$rs2), (MULH   GPR:$rs1, GPR:$rs2)>;
def : Pat<(mulhu  GPR:$rs1, GPR:$rs2), (MULHU  GPR:$rs1, GPR:$rs2)>;

} // Predicates = [HasStdExtM]

//===----------------------------------------------------------------------===//
// SYSTEM: environment calls
//===----------------------------------------------------------------------===//
//
// Chapter 12: the runtime needs a way to talk to the outside world.  ECALL and
// EBREAK are I-type encodings with rs1 = rd = 0 and imm = 0 / 1, so they fit
// in the existing I-type format without operands; the whole 32-bit word is
// fixed here.  `ecall` traps to the environment (a Linux syscall under QEMU),
// `ebreak` to a debugger.

def ECALL : RVMinInst<(outs), (ins), "ecall", []> {
  let Inst = 0x00000073;
  let isBarrier = 1;
}

def EBREAK : RVMinInst<(outs), (ins), "ebreak", []> {
  let Inst = 0x00100073;
}


//===----------------------------------------------------------------------===//

// PseudoRET is expanded by TableGen's pseudo-lowering emitter (chapter 2's MC
// layer is reused): it prints and encodes as `jalr zero, 0(ra)`.
def PseudoRET : RVMinPseudo<(outs), (ins), "", [(RVMinRet)]>,
                PseudoInstExpansion<(JALR X0, X1, 0)> {
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
  let Size = 4;
}

// Unconditional branch: JAL with rd = x0 is the RV32I `j` form.  Keeping a
// pseudo with no result lets the register allocator leave rd alone, and the
// asm printer expands it to `jal zero, target`.
def PseudoBR : RVMinPseudo<(outs), (ins simm21_lsb0:$imm21), "", []>,
               PseudoInstExpansion<(JAL X0, simm21_lsb0:$imm21)> {
  let isBranch = 1;
  let isBarrier = 1;
  let isTerminator = 1;
  let Size = 4;
}
def : Pat<(br bb:$dst), (PseudoBR simm21_lsb0:$dst)>;

// ADJCALLSTACKDOWN/UP bracket every call in the DAG (CALLSEQ_START/END).  They
// carry the size of the outgoing argument area.  RVMin has a reserved call
// frame, so PEI deletes them again: the area is part of the fixed stack frame
// from the prologue on (see RVMinFrameLowering::eliminateCallFramePseudoInstr).
// The patterns on the two pseudos are what selects CALLSEQ_START/END; the
// explicit stack-pointer use/def tells the scheduler that the pair may move sp
// (it does not in RVMin, but the DAG is built before frame lowering decides).
let Defs = [X2], Uses = [X2] in {
def ADJCALLSTACKDOWN : RVMinPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2), "",
                                   [(callseq_start timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP : RVMinPseudo<(outs), (ins i32imm:$amt1, i32imm:$amt2), "",
                                 [(callseq_end timm:$amt1, timm:$amt2)]>;
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

// Direct call.  The asm printer expands it into three real instructions:
//
//   lui  t0, %hi(callee)
//   addi t0, t0, %lo(callee)
//   jalr ra, t0, 0
//
// Materializing the absolute address reuses the chapter 7 %hi/%lo pair and its
// relocations, so a call needs no new MC support.  The real RISC-V ABI uses
// AUIPC+JALR with R_RISCV_CALL so the linker can relax the pair to a single
// `jal`; that needs linker relaxation, which RVMin leaves for later.
//
// Defs = [X1] is what makes `ra` appear modified: PEI then saves it in the
// prologue of every function that contains a call.
let isCall = 1, Defs = [X1], Size = 12 in
def PseudoCALL : RVMinPseudo<(outs), (ins bare_symbol:$func), "", []>;

def : Pat<(RVMinCall tglobaladdr:$func), (PseudoCALL tglobaladdr:$func)>;
def : Pat<(RVMinCall texternalsym:$func), (PseudoCALL texternalsym:$func)>;

// Indirect call: the target address is already in a register, so a single
// `jalr ra, rs1, 0` is enough.  TableGen's pseudo-lowering emitter handles the
// expansion because it fits in one instruction.
let isCall = 1, Defs = [X1], Size = 4 in
def PseudoCALLR : RVMinPseudo<(outs), (ins GPR:$rs1), "", []>,
                  PseudoInstExpansion<(JALR X1, GPR:$rs1, 0)>;

def : Pat<(RVMinCall GPR:$rs1), (PseudoCALLR GPR:$rs1)>;

//===----------------------------------------------------------------------===//
// Chapter 17: tail calls
//===----------------------------------------------------------------------===//
//
// A tail call is a call whose result is returned immediately.  Since the
// epilogue already restored sp and every callee-saved register, the callee can
// return directly to *our* caller, so this is a plain jump:
//
//   lui   t0, %hi(callee)      (direct callee; same %hi/%lo pair as a call)
//   addi  t0, t0, %lo(callee)
//   jalr  zero, 0(t0)          rd = x0: no return address link
//
// `isReturn` is what makes PEI emit the epilogue in this block: like PseudoRET
// it ends the function, but the register that carries the return address is
// the caller's `ra`, which we never touch.

let isTerminator = 1, isBarrier = 1, isReturn = 1, Size = 12 in
def PseudoTAIL : RVMinPseudo<(outs), (ins bare_symbol:$func), "", []>;

def : Pat<(RVMinTail tglobaladdr:$func), (PseudoTAIL tglobaladdr:$func)>;
def : Pat<(RVMinTail texternalsym:$func), (PseudoTAIL texternalsym:$func)>;

// Indirect tail call: the address is already in a register, so `jalr zero,
// rs1, 0` is the whole instruction.
let isTerminator = 1, isBarrier = 1, isReturn = 1, Size = 4 in
def PseudoTAILIND : RVMinPseudo<(outs), (ins GPR:$rs1), "", []>,
                    PseudoInstExpansion<(JALR X0, GPR:$rs1, 0)>;

def : Pat<(RVMinTail GPR:$rs1), (PseudoTAILIND GPR:$rs1)>;

// Chapter 18: the second half of a jump table.  BR_JT is lowered to a load of
// the entry followed by BRIND, and BRIND is exactly `jalr zero, 0(rs)` -- the
// same encoding as an indirect tail call, but reached from a table instead of
// from a `musttail` call.  Unlike a call it is a branch: no link, no return.
def PseudoBRIND : RVMinPseudo<(outs), (ins GPR:$rs1), "", []>,
                  PseudoInstExpansion<(JALR X0, GPR:$rs1, 0)> {
  let isBranch = 1;
  let isBarrier = 1;
  let isTerminator = 1;
  // The target is in a register, not in the instruction stream: this matters
  // to AsmPrinter's fallthrough analysis, which must keep the label of a jump
  // table's first block (it is a layout successor of the indirect jump).
  let isIndirectBranch = 1;
  let Size = 4;
}

def : Pat<(brind GPR:$rs1), (PseudoBRIND GPR:$rs1)>;
