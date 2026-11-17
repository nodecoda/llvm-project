//===-- RVMinInstrInfoV.td - Vector extension instructions ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 47: the V-extension MC layer.
//
// This is the Zve32x teaching subset, not the full RVV:
//
//   * SEW = 32, LMUL = 1, VLEN = 128, integer only;
//   * no masks (vm is fixed to 1), no widening/narrowing, no vector FP;
//   * v0 is an ordinary vector register here because no instruction masks.
//
// The nineteen instructions are enough to configure VL, move whole vectors
// and do integer arithmetic/load/store.  Code generation starts in chapter 48.
//
// Encoding recap:
//
//   vsetivli 31..30=11 | vtypei[9:0] | uimm5 | 111 | rd | 1010111
//   vsetvli  31=0     | vtypei[10:0]          | rs1   | 111 | rd | 1010111
//   vsetvl   1000000                          | rs2   | rs1   | 111 | rd | 1010111
//   OP-V     funct6 | vm | vs2 | vs1/rs1/imm | funct3 | vd | 1010111
//   vle32.v  nf=0 | mew=0 | mop=00 | vm=1 | lumop=0 | rs1 | 110 | vd | 0000111
//   vse32.v  nf=0 | mew=0 | mop=00 | vm=1 | sumop=0 | rs1 | 110 | vs3 | 0100111
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Operands
//===----------------------------------------------------------------------===//

// The vtype operand is spelled as a comma-separated group of tokens
// (`e32, m1, ta, ma`) or as a raw integer.  parseVType folds both spellings
// into one immediate; the two match classes differ only in the number of bits
// the instruction can encode (10 for vsetivli, 11 for vsetvli).
class RVMinVTypeAsmOperand<string name> : AsmOperandClass {
  let Name = name;
  let ParserMethod = "parseVType";
  let DiagnosticType = "InvalidVType";
  let RenderMethod = "addImmOperands";
}

def VTypeI10AsmOperand : RVMinVTypeAsmOperand<"VTypeI10">;
def VTypeI11AsmOperand : RVMinVTypeAsmOperand<"VTypeI11">;

def vtypei10 : Operand<i32> {
  let PrintMethod = "printVTypeI";
  let ParserMatchClass = VTypeI10AsmOperand;
}

def vtypei11 : Operand<i32> {
  let PrintMethod = "printVTypeI";
  let ParserMatchClass = VTypeI11AsmOperand;
}

// 5-bit signed immediate: vadd.vi/vmv.v.i and friends.
class RVMinVSImmAsmOperand<string name, string predicate> : AsmOperandClass {
  let Name = name;
  let PredicateMethod = predicate;
  let RenderMethod = "addImmOperands";
}

def SImm5AsmOperand : RVMinVSImmAsmOperand<"SImm5", "isSImm5">;

def simm5 : Operand<i32>, ImmLeaf<i32, [{ return isInt<5>(Imm); }]> {
  let PrintMethod = "printOperand";
  let ParserMatchClass = SImm5AsmOperand;
  let DecoderMethod = "decodeSImmOperand<5>";
  let MCOperandPredicate = [{
    int64_t Imm;
    return MCOp.evaluateAsConstantImm(Imm) && isInt<5>(Imm);
  }];
}

//===----------------------------------------------------------------------===//
// Configuration: vsetivli / vsetvli / vsetvl
//===----------------------------------------------------------------------===//
//
// All three update VL and VTYPE, so they are side-effecting.  The immediate
// forms let the assembler state SEW/LMUL/tail/mask policy directly; the
// register form takes the vtype from rs2.

class RVMinVSetivliInst : RVMinInst<(outs GPR:$rd),
                                    (ins uimm5:$uimm, vtypei10:$vtypei),
                                    "vsetivli $rd, $uimm, $vtypei", []> {
  bits<5> rd;
  bits<5> uimm;
  bits<10> vtypei;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = 0b111;
  let Inst{19-15} = uimm;
  let Inst{29-20} = vtypei;
  let Inst{30}    = 1;
  let Inst{31}    = 1;

  let TSFlags{4-0} = FmtV.Value;
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

class RVMinVSetvliInst : RVMinInst<(outs GPR:$rd),
                                   (ins GPR:$rs1, vtypei11:$vtypei),
                                   "vsetvli $rd, $rs1, $vtypei", []> {
  bits<5> rd;
  bits<5> rs1;
  bits<11> vtypei;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = 0b111;
  let Inst{19-15} = rs1;
  let Inst{30-20} = vtypei;
  let Inst{31}    = 0;

  let TSFlags{4-0} = FmtV.Value;
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

class RVMinVSetvlInst : RVMinInst<(outs GPR:$rd),
                                  (ins GPR:$rs1, GPR:$rs2),
                                  "vsetvl $rd, $rs1, $rs2", []> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = 0b111;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{31-25} = 0b1000000;

  let TSFlags{4-0} = FmtV.Value;
  let hasSideEffects = 1;
  let mayLoad = 0;
  let mayStore = 0;
}

//===----------------------------------------------------------------------===//
// Vector memory: vle32.v / vse32.v
//===----------------------------------------------------------------------===//
//
// Unit-stride, unmasked, 32-bit elements.  nf=0 (one field), mew=0, mop=00,
// vm=1 and lumop/sumop=0 are all fixed.  The width field 110 is SEW=32.

class RVMinVLoadInst : RVMinInst<(outs VR:$vd), (ins GPR:$rs1),
                                 "vle32.v $vd, (${rs1})", []> {
  bits<5> vd;
  bits<5> rs1;

  let Inst{6-0}   = OPC_VLOAD.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = 0b110;
  let Inst{19-15} = rs1;
  let Inst{24-20} = 0b00000;
  let Inst{25}    = 1;
  let Inst{27-26} = 0b00;
  let Inst{28}    = 0;
  let Inst{31-29} = 0b000;

  let TSFlags{4-0} = FmtV.Value;
  let mayLoad = 1;
  let mayStore = 0;
}

class RVMinVStoreInst : RVMinInst<(outs), (ins VR:$vs3, GPR:$rs1),
                                  "vse32.v $vs3, (${rs1})", []> {
  bits<5> vs3;
  bits<5> rs1;

  let Inst{6-0}   = OPC_VSTORE.Value;
  let Inst{11-7}  = vs3;
  let Inst{14-12} = 0b110;
  let Inst{19-15} = rs1;
  let Inst{24-20} = 0b00000;
  let Inst{25}    = 1;
  let Inst{27-26} = 0b00;
  let Inst{28}    = 0;
  let Inst{31-29} = 0b000;

  let TSFlags{4-0} = FmtV.Value;
  let mayLoad = 0;
  let mayStore = 1;
}

//===----------------------------------------------------------------------===//
// OP-V register/immediate forms
//===----------------------------------------------------------------------===//
//
// funct3 selects the operand class:
//
//   .vv -> OPIVV (000) or OPMVV (010) for integer multiply
//   .vx -> OPIVX (100) or OPMVX (110) for integer multiply
//   .vi -> OPIVI (011)
//
// vm (bit 25) is 1: every instruction in this subset is unmasked.

class RVMinVVInst<bits<6> funct6, bits<3> funct3, string mnemonic>
    : RVMinInst<(outs VR:$vd), (ins VR:$vs2, VR:$vs1),
                mnemonic # " $vd, $vs2, $vs1", []> {
  bits<5> vd;
  bits<5> vs2;
  bits<5> vs1;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = vs1;
  let Inst{24-20} = vs2;
  let Inst{25}    = 1;
  let Inst{31-26} = funct6;

  let TSFlags{4-0} = FmtV.Value;
}

class RVMinVXInst<bits<6> funct6, bits<3> funct3, string mnemonic>
    : RVMinInst<(outs VR:$vd), (ins VR:$vs2, GPR:$rs1),
                mnemonic # " $vd, $vs2, $rs1", []> {
  bits<5> vd;
  bits<5> vs2;
  bits<5> rs1;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = vs2;
  let Inst{25}    = 1;
  let Inst{31-26} = funct6;

  let TSFlags{4-0} = FmtV.Value;
}

class RVMinVIInst<bits<6> funct6, bits<3> funct3, string mnemonic>
    : RVMinInst<(outs VR:$vd), (ins VR:$vs2, simm5:$imm),
                mnemonic # " $vd, $vs2, $imm", []> {
  bits<5> vd;
  bits<5> vs2;
  bits<5> imm;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = imm;
  let Inst{24-20} = vs2;
  let Inst{25}    = 1;
  let Inst{31-26} = funct6;

  let TSFlags{4-0} = FmtV.Value;
}

let Predicates = [HasStdExtV] in {
def VSETIVLI : RVMinVSetivliInst;
def VSETVLI  : RVMinVSetvliInst;
def VSETVL   : RVMinVSetvlInst;

def VLE32_V : RVMinVLoadInst;
def VSE32_V : RVMinVStoreInst;

def VADD_VV : RVMinVVInst<0b000000, 0b000, "vadd.vv">;
def VADD_VX : RVMinVXInst<0b000000, 0b100, "vadd.vx">;
def VADD_VI : RVMinVIInst<0b000000, 0b011, "vadd.vi">;

def VSUB_VV : RVMinVVInst<0b000010, 0b000, "vsub.vv">;
def VSUB_VX : RVMinVXInst<0b000010, 0b100, "vsub.vx">;

// Integer multiply lives in the OPMVV/OPMVX encoding space (funct3 010/110).
def VMUL_VV : RVMinVVInst<0b100101, 0b010, "vmul.vv">;
def VMUL_VX : RVMinVXInst<0b100101, 0b110, "vmul.vx">;

def VAND_VV : RVMinVVInst<0b001001, 0b000, "vand.vv">;
def VAND_VX : RVMinVXInst<0b001001, 0b100, "vand.vx">;
def VAND_VI : RVMinVIInst<0b001001, 0b011, "vand.vi">;
def VOR_VV  : RVMinVVInst<0b001010, 0b000, "vor.vv">;
def VOR_VX  : RVMinVXInst<0b001010, 0b100, "vor.vx">;
def VOR_VI  : RVMinVIInst<0b001010, 0b011, "vor.vi">;
def VXOR_VV : RVMinVVInst<0b001011, 0b000, "vxor.vv">;
def VXOR_VX : RVMinVXInst<0b001011, 0b100, "vxor.vx">;
def VXOR_VI : RVMinVIInst<0b001011, 0b011, "vxor.vi">;

// Chapter 49: vredsum.vs vd, vs2, vs1 sums the elements of vs2 and adds
// vs1[0], writing the scalar result to vd[0].  It lives in OPMVV (funct3 010)
// with funct6 000000, like vadd but in the multiply/other space.
def VREDSUM_VS : RVMinVVInst<0b000000, 0b010, "vredsum.vs">;

//===----------------------------------------------------------------------===//
// Moves
//===----------------------------------------------------------------------===//
//
// vmv.v.v / vmv.v.i broadcast a scalar (register or 5-bit immediate) into
// every active element; vs2 is zero because the operation is unary.
//
// vmv.x.s / vmv.s.x move one element between a vector register and an integer
// register.  They use the OPMVV/OPMVX encoding spaces with a fixed zero
// vs1/vs2 field.

class RVMinVMVVIInst : RVMinInst<(outs VR:$vd), (ins VR:$vs1),
                                 "vmv.v.v $vd, $vs1", []> {
  bits<5> vd;
  bits<5> vs1;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = 0b000;
  let Inst{19-15} = vs1;
  let Inst{24-20} = 0b00000;
  let Inst{25}    = 1;
  let Inst{31-26} = 0b010111;

  let TSFlags{4-0} = FmtV.Value;
}

class RVMinVMVVIiInst : RVMinInst<(outs VR:$vd), (ins simm5:$imm),
                                  "vmv.v.i $vd, $imm", []> {
  bits<5> vd;
  bits<5> imm;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = 0b011;
  let Inst{19-15} = imm;
  let Inst{24-20} = 0b00000;
  let Inst{25}    = 1;
  let Inst{31-26} = 0b010111;

  let TSFlags{4-0} = FmtV.Value;
}

// vmv.v.x broadcasts an integer register into every active element.  It is
// OPIVX (funct3 100): the vs2 field is fixed to zero and rs1 carries the
// source, unlike vmv.s.x which uses OPMVX (funct3 110).
class RVMinVMVVXInst : RVMinInst<(outs VR:$vd), (ins GPR:$rs1),
                                 "vmv.v.x $vd, $rs1", []> {
  bits<5> vd;
  bits<5> rs1;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = 0b100;
  let Inst{19-15} = rs1;
  let Inst{24-20} = 0b00000;
  let Inst{25}    = 1;
  let Inst{31-26} = 0b010111;

  let TSFlags{4-0} = FmtV.Value;
}

class RVMinVMVXSInst : RVMinInst<(outs GPR:$rd), (ins VR:$vs2),
                                 "vmv.x.s $rd, $vs2", []> {
  bits<5> rd;
  bits<5> vs2;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = rd;
  let Inst{14-12} = 0b010;
  let Inst{19-15} = 0b00000;
  let Inst{24-20} = vs2;
  let Inst{25}    = 1;
  let Inst{31-26} = 0b010000;

  let TSFlags{4-0} = FmtV.Value;
}

class RVMinVMVSXInst : RVMinInst<(outs VR:$vd), (ins GPR:$rs1),
                                 "vmv.s.x $vd, $rs1", []> {
  bits<5> vd;
  bits<5> rs1;

  let Inst{6-0}   = OPC_OP_V.Value;
  let Inst{11-7}  = vd;
  let Inst{14-12} = 0b110;
  let Inst{19-15} = rs1;
  let Inst{24-20} = 0b00000;
  let Inst{25}    = 1;
  let Inst{31-26} = 0b010000;

  let TSFlags{4-0} = FmtV.Value;
}

def VMV_V_V : RVMinVMVVIInst;
def VMV_V_X : RVMinVMVVXInst;
def VMV_V_I : RVMinVMVVIiInst;
def VMV_X_S : RVMinVMVXSInst;
def VMV_S_X : RVMinVMVSXInst;

// Chapter 48: integer arithmetic on <4 x i32>.  Only the .vv forms are
// selected for now; .vx/.vi are left for the next chapter's constant and
// scalar-broadcast optimizations.
def : Pat<(v4i32 (add VR:$vs2, VR:$vs1)), (VADD_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (sub VR:$vs2, VR:$vs1)), (VSUB_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (mul VR:$vs2, VR:$vs1)), (VMUL_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (and VR:$vs2, VR:$vs1)), (VAND_VV VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (or  VR:$vs2, VR:$vs1)), (VOR_VV  VR:$vs2, VR:$vs1)>;
def : Pat<(v4i32 (xor VR:$vs2, VR:$vs1)), (VXOR_VV VR:$vs2, VR:$vs1)>;
} // Predicates = [HasStdExtV]
