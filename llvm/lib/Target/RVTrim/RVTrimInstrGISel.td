//===-- RVTrimInstrGISel.td - RISC-V GISel target pseudos ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
// RISC-V GlobalISel target pseudo instruction definitions. This is kept
// separately from the other tablegen files for organizational purposes, but
// share the same infrastructure.
//
//===----------------------------------------------------------------------===//

class RVTrimGenericInstruction : GenericInstruction {
  let Namespace = "RVTrim";
}

// Pseudo equivalent to a RVTrimISD::SRAW.
def G_SRAW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_SRAW, rvtrim_sraw>;

// Pseudo equivalent to a RVTrimISD::SRLW.
def G_SRLW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_SRLW, rvtrim_srlw>;

// Pseudo equivalent to a RVTrimISD::SLLW.
def G_SLLW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_SLLW, rvtrim_sllw>;

// Pseudo equivalent to a RVTrimISD::DIVW.
def G_DIVW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_DIVW, rvtrim_divw>;

// Pseudo equivalent to a RVTrimISD::DIVUW.
def G_DIVUW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_DIVUW, rvtrim_divuw>;

// Pseudo equivalent to a RVTrimISD::REMUW.
def G_REMUW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_REMUW, rvtrim_remuw>;

// Pseudo equivalent to a RVTrimISD::RORW.
def G_RORW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_RORW, rvtrim_rorw>;

// Pseudo equivalent to a RVTrimISD::ROLW.
def G_ROLW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_ROLW, rvtrim_rolw>;

// Pseudo equivalent to a RVTrimISD::BREV8.
def G_BREV8 : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src);
  let hasSideEffects = false;
}

def : GINodeEquiv<G_BREV8, rvtrim_brev8>;

// Pseudo equivalent to a RVTrimISD::CLZW.
def G_CLZW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_CLZW, rvtrim_clzw>;

// Pseudo equivalent to a RVTrimISD::CTZW.
def G_CTZW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_CTZW, rvtrim_ctzw>;

// Pseudo equivalent to a RVTrimISD::FCVT_W_RV64.
def G_FCVT_W_RV64 : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$src, untyped_imm_0:$frm);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_FCVT_W_RV64, rvtrim_fcvt_w_rv64>;

// Pseudo equivalent to a RVTrimISD::FCVT_WU_RV64.
def G_FCVT_WU_RV64 : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$src, untyped_imm_0:$frm);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_FCVT_WU_RV64, rvtrim_fcvt_wu_rv64>;

// Pseudo equivalent to a RVTrimISD::FCVT_X. Corresponds to fcvt.w(u).* on RV32
// and fcvt.l(u).* on RV64. Saturates out-of-range inputs. Rounding mode is
// passed as an untyped immediate using the RVTrimFPRndMode enum.
def G_FCVT_X : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$src, untyped_imm_0:$frm);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_FCVT_X, rvtrim_fcvt_x>;

// Pseudo equivalent to a RVTrimISD::FCVT_XU. Corresponds to fcvt.wu.* on RV32
// and fcvt.lu.* on RV64. Saturates out-of-range inputs. Rounding mode is
// passed as an untyped immediate using the RVTrimFPRndMode enum.
def G_FCVT_XU : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$src, untyped_imm_0:$frm);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_FCVT_XU, rvtrim_fcvt_xu>;

// Pseudo equivalent to a RVTrimISD::FCLASS.
def G_FCLASS : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$src);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_FCLASS, rvtrim_fclass>;

// Pseudo equivalent to a RVTrimISD::READ_VLENB.
def G_READ_VLENB : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_READ_VLENB, rvtrim_read_vlenb>;

// Pseudo equivalent to a RVTrimISD::VMCLR_VL
def G_VMCLR_VL : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$vl);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_VMCLR_VL, rvtrim_vmclr_vl>;

// Pseudo equivalent to a RVTrimISD::VMSET_VL
def G_VMSET_VL : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type1:$vl);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_VMSET_VL, rvtrim_vmset_vl>;

def G_SPLAT_VECTOR_SPLIT_I64_VL : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$passthru, type1:$hi, type1:$lo, type2:$vl);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_SPLAT_VECTOR_SPLIT_I64_VL, rvtrim_splat_vector_split_i64_vl>;

// Pseudo equivalent to a RVTrimISD::VSLIDEDOWN_VL
def G_VSLIDEDOWN_VL : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$merge, type0:$vec, type1:$idx, type2:$mask,
                       type1:$vl, type1:$policy);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_VSLIDEDOWN_VL, rvtrim_slidedown_vl>;

// Pseudo equivalent to a RVTrimISD::VMV_V_V_VL
def G_VMV_V_V_VL : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$passthru, type0:$vec, type1:$vl);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_VMV_V_V_VL, rvtrim_vmv_v_v_vl>;

// Pseudo equivalent to a RVTrimISD::VSLIDEUP_VL
def G_VSLIDEUP_VL : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$merge, type0:$vec, type1:$idx, type2:$mask,
                       type3:$vl, type4:$policy);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_VSLIDEUP_VL, rvtrim_slideup_vl>;

// Pseudo equivalent to RVTrimISD::CLSW
def G_CLSW : RVTrimGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins type0:$src);
  let hasSideEffects = false;
}
def : GINodeEquiv<G_CLSW , rvtrim_clsw>;

// Pseudo equivalent to a software guarded indirect branch,
// RVTrimISD::SW_GUARDED_BRIND.
def G_SW_GUARDED_BRIND : RVTrimGenericInstruction {
  let OutOperandList = (outs);
  let InOperandList = (ins ptype0:$target);
  let hasSideEffects = false;
  let isBranch = true;
  let isTerminator = true;
  let isBarrier = true;
  let isIndirectBranch = true;
}
def : GINodeEquiv<G_SW_GUARDED_BRIND, rvtrim_sw_guarded_brind>;
