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

/// The 11-bit displacement of the M format (chapter 06).  Splitting it from
/// simm16 is what makes the assembler reject `ld.global.b32 r1, 2000(r2)`: the
/// field is not wide enough, and silently truncating it would be worse than
/// refusing to assemble.
def SImm11AsmOperand : GPUMinImmAsmOperand<"SImm11", "isSImm11">;

def simm11 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm11AsmOperand;
  let DecoderMethod = "DecodeSImm11";
}

/// The matching side of simm11: an ImmLeaf is what a *pattern* can range-check,
/// because by the time SelectionDAG matches, the operand is a constant node
/// rather than assembly text.  Patterns that fold an immediate into the
/// displacement use this leaf, so a large offset falls back to a register add
/// instead of being truncated.
def simm11imm : ImmLeaf<i32, [{ return isInt<11>(Imm); }]>;

/// A branch target.  The assembly text is a label (or a word offset); the
/// encoder turns it into a PC-relative 16-bit displacement, which is why the
/// operand type is OtherVT: it never becomes a value in a register.
/// A call target: the assembly text must be a symbol, because the J-format
/// offset is PC-relative and only the layout step can resolve it.  Unlike a
/// branch the value never becomes a register, so the operand type is i32 to
/// let a TargetGlobalAddress flow straight from lowering.
def CallTargetAsmOperand : GPUMinImmAsmOperand<"CallTarget", "isCallTarget">;

def calltarget21 : Operand<i32> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = CallTargetAsmOperand;
  let EncoderMethod = "getCallTargetOpValue";
  let DecoderMethod = "DecodeCallTarget21";
}

/// The range-checked sibling of simm16 for patterns.
def simm16imm : ImmLeaf<i32, [{ return isInt<16>(Imm); }]>;

def brtarget16 : Operand<OtherVT> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = BranchTargetAsmOperand;
  let EncoderMethod = "getBranchTargetOpValue";
  let DecoderMethod = "DecodeBrTarget16";
}

// The format classes are included here, after the operand classes: their
// operand lists name `simm11`, and TableGen resolves names in definition order.
include "GPUMinInstrFormats.td"

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

// Chapter 07: `addi` is its own opcode rather than an "ALU immediate" bit.
// The I format spends all 16 low bits on the immediate, so there is no funct
// field left to say which ALU operation an immediate form means; a separate
// opcode is the cheapest honest answer.
def ADDI : GPUMinI<OPC_ADDI.Value, (outs GPR:$rd), (ins GPR:$rs1, simm16:$imm),
                   "addi $rd, $rs1, $imm", []>;

def : Pat<(add GPR:$rs1, simm16imm:$imm), (ADDI $rs1, simm16imm:$imm)>;

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
//
// M format (GPUMinInstrFormats.td):
//
//   [31:26] opcode | [25:21] rd/rs2 | [20:16] base | [15:5] imm11
//   [4:3] space | [2:1] size | [0] sign
//
// Every space/size combination is a distinct opcode, not a sub-field the
// assembler parses at run time: that is what lets the generated decoder pick
// `ld.shared.u16` out of a word without any hand-written bit tests.
//
// Only `global` (space 0) is selected from IR in this chapter.  The shared,
// local and constant forms already encode and decode, so chapter 16 (shared
// memory) only has to add lowering, not encoding.

multiclass GPUMinMemLoads<string space, bits<2> sp> {
  def _B32 : GPUMinLdM<OPC_LD.Value, sp, 0b10, 0,
                       "ld."#space#".b32 $rd, ${imm}(${base})">;
  def _U8  : GPUMinLdM<OPC_LD.Value, sp, 0b00, 0,
                       "ld."#space#".u8 $rd, ${imm}(${base})">;
  def _S8  : GPUMinLdM<OPC_LD.Value, sp, 0b00, 1,
                       "ld."#space#".s8 $rd, ${imm}(${base})">;
  def _U16 : GPUMinLdM<OPC_LD.Value, sp, 0b01, 0,
                       "ld."#space#".u16 $rd, ${imm}(${base})">;
  def _S16 : GPUMinLdM<OPC_LD.Value, sp, 0b01, 1,
                       "ld."#space#".s16 $rd, ${imm}(${base})">;
}

multiclass GPUMinMemStores<string space, bits<2> sp> {
  def _B32 : GPUMinStM<OPC_ST.Value, sp, 0b10,
                       "st."#space#".b32 $rs2, ${imm}(${base})">;
  def _B8  : GPUMinStM<OPC_ST.Value, sp, 0b00,
                       "st."#space#".b8 $rs2, ${imm}(${base})">;
  def _B16 : GPUMinStM<OPC_ST.Value, sp, 0b01,
                       "st."#space#".b16 $rs2, ${imm}(${base})">;
}

defm LD_GLOBAL  : GPUMinMemLoads<"global",  0b00>;
defm LD_SHARED  : GPUMinMemLoads<"shared",  0b01>;
defm LD_LOCAL   : GPUMinMemLoads<"local",   0b10>;
defm LD_CONST   : GPUMinMemLoads<"constant", 0b11>;
defm ST_GLOBAL  : GPUMinMemStores<"global", 0b00>;
defm ST_SHARED  : GPUMinMemStores<"shared", 0b01>;
defm ST_LOCAL   : GPUMinMemStores<"local",  0b10>;

// Selecting a 32-bit load: the address is either already in a register (offset
// zero) or a base plus a displacement that fits the 11-bit field.  Anything
// else stays a register add and reaches the load through the first pattern.
def : Pat<(load GPR:$base), (LD_GLOBAL_B32 $base, 0)>;
def : Pat<(load (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_B32 $base, simm11imm:$off)>;

def : Pat<(store GPR:$val, GPR:$base), (ST_GLOBAL_B32 $val, $base, 0)>;
def : Pat<(store GPR:$val, (add GPR:$base, simm11imm:$off)),
          (ST_GLOBAL_B32 $val, $base, simm11imm:$off)>;

// Chapter 08: narrow loads and stores.  i8/i16 are promoted to i32, so the
// memory operations arrive as extloads and truncstores.  Each extension kind
// has its own opcode: using a 32-bit load plus a shift would read three bytes
// too many (and fault across a page boundary).
def : Pat<(zextloadi8 GPR:$base), (LD_GLOBAL_U8 $base, 0)>;
def : Pat<(sextloadi8 GPR:$base), (LD_GLOBAL_S8 $base, 0)>;
def : Pat<(extloadi8 GPR:$base),  (LD_GLOBAL_U8 $base, 0)>;
def : Pat<(zextloadi8 (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_U8 $base, simm11imm:$off)>;
def : Pat<(sextloadi8 (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_S8 $base, simm11imm:$off)>;
def : Pat<(extloadi8 (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_U8 $base, simm11imm:$off)>;

def : Pat<(zextloadi16 GPR:$base), (LD_GLOBAL_U16 $base, 0)>;
def : Pat<(sextloadi16 GPR:$base), (LD_GLOBAL_S16 $base, 0)>;
def : Pat<(extloadi16 GPR:$base),  (LD_GLOBAL_U16 $base, 0)>;
def : Pat<(zextloadi16 (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_U16 $base, simm11imm:$off)>;
def : Pat<(sextloadi16 (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_S16 $base, simm11imm:$off)>;
def : Pat<(extloadi16 (add GPR:$base, simm11imm:$off)),
          (LD_GLOBAL_U16 $base, simm11imm:$off)>;

def : Pat<(truncstorei8 GPR:$val, GPR:$base),
          (ST_GLOBAL_B8 $val, $base, 0)>;
def : Pat<(truncstorei8 GPR:$val, (add GPR:$base, simm11imm:$off)),
          (ST_GLOBAL_B8 $val, $base, simm11imm:$off)>;
def : Pat<(truncstorei16 GPR:$val, GPR:$base),
          (ST_GLOBAL_B16 $val, $base, 0)>;
def : Pat<(truncstorei16 GPR:$val, (add GPR:$base, simm11imm:$off)),
          (ST_GLOBAL_B16 $val, $base, simm11imm:$off)>;

//===----------------------------------------------------------------------===//
// Calls (opcode 0x11)
//===----------------------------------------------------------------------===//
//
// J format: opcode[31:26] | 0[25:21] | offset21[20:0], offset in words.
//
// `call` writes the return address into lr (r31) and jumps.  The register is
// implicit: there is no `call rd, target` form, which is why LR is reserved in
// GPUMinRegisterInfo rather than allocated.
def CALL : GPUMinJ<OPC_CALL.Value, (outs), (ins calltarget21:$off),
                   "call $off", []> {
  let isCall = 1;
  let isTerminator = 0;
  let hasSideEffects = 1;
  // lr is written by the call itself.  The argument registers and the
  // call-preserved mask travel as extra operands of the CALL node; the
  // regmask is what tells the register allocator which registers survive.
  let Defs = [R31];
}

def SDT_GPUMinCall : SDTypeProfile<0, -1, [SDTCisVT<0, i32>]>;
def GPUMinCall : SDNode<"GPUMinISD::CALL", SDT_GPUMinCall,
                        [SDNPHasChain, SDNPOutGlue, SDNPOptInGlue,
                         SDNPVariadic]>;

def : Pat<(GPUMinCall tglobaladdr:$dst), (CALL tglobaladdr:$dst)>;

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
  // Not a barrier: the false edge is the layout fallthrough, which
  // analyzeBranch() reports with FBB == nullptr.
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

// Chapter 07: a device function returns to lr.  Kernels still `exit`; the
// choice is made in LowerReturn from the "gpumin_kernel" attribute, which is
// why there are two return pseudos instead of one.
def RET : GPUMinInst<(outs), (ins), "ret", []> {
  let Inst{31-26} = OPC_SYS.Value;
  let Inst{10-0} = 1;
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
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
def GPUMinKernelRet : SDNode<"GPUMinISD::KRET", SDTNone,
                             [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

def PseudoRET : GPUMinPseudo<(outs), (ins), "", [(GPUMinRet)]>,
                PseudoInstExpansion<(RET)> {
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
}

def PseudoEXIT : GPUMinPseudo<(outs), (ins), "", [(GPUMinKernelRet)]>,
                 PseudoInstExpansion<(EXIT)> {
  let isReturn = 1;
  let isBarrier = 1;
  let isTerminator = 1;
}
