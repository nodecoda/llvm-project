//===-- RVTrimInstrInfoV.td - RISC-V 'V' instructions -------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// This file describes the RISC-V instructions from the standard 'V' Vector
/// extension, version 1.0.
///
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Scheduling definitions.
//===----------------------------------------------------------------------===//

// Common class of scheduling definitions.
// `ReadVPassthru` will be prepended to reads if instruction is masked.
// `ReadVMask` will be appended to reads if instruction is masked.
// Operands:
//   `writes`       SchedWrites that are listed for each explicit def operand
//                  in order.
//   `reads`        SchedReads that are listed for each explicit use operand.
//   `forceMasked`  Forced to be masked (e.g. Add-with-Carry Instructions).
//   `forcePassthruRead` Force to have read for passthru operand.
class SchedCommon<list<SchedWrite> writes, list<SchedRead> reads,
                  string mx = "WorstCase", int sew = 0, bit forceMasked = 0,
                  bit forcePassthruRead = 0> : Sched<[]> {
  defvar isMasked = !ne(!find(NAME, "_MASK"), -1);
  defvar isTied = !ne(!find(NAME, "_TIED"), -1);
  defvar isMaskedOrForceMasked = !or(forceMasked, isMasked);
  defvar isTiedMasked = !and(isMaskedOrForceMasked, isTied);
  defvar passthruRead = !if(!or(!eq(mx, "WorstCase"), !eq(sew, 0)),
                            !cast<SchedRead>("ReadVPassthru_" # mx),
                            !cast<SchedRead>("ReadVPassthru_" # mx # "_E" #sew));
  // We don't need passthru operand if it's already _TIED without mask.
  defvar needsForcePassthruRead = !and(forcePassthruRead, !not(isTied));
  defvar needsPassthruRead = !or(isMaskedOrForceMasked, needsForcePassthruRead);
  // If this is a _TIED + masked operation, $rs2 (i.e. the first operand) is
  // merged with the mask.
  // NOTE: the following if statement is written in such a weird way because
  // should we want to write something like
  // `!if(!and(!not(!empty(reads), isTiedMasked), !tail(reads), reads)`
  // since `!if` doesn't have a proper short-circuit behavior, if the
  // condition of this `!if` cannot be resolved right away, `!tail(reads)` will
  // be immediately evaluated anyway even when `reads` is empty, which leads to
  // an assertion failure.
  defvar readsWithTiedMask =
      !if(isTiedMasked, !if(!not(!empty(reads)), !tail(reads), reads), reads);
  defvar readsWithMask =
      !if(isMaskedOrForceMasked, !listconcat(readsWithTiedMask, [ReadVMask]), reads);
  defvar allReads =
      !if(needsPassthruRead, !listconcat([passthruRead], readsWithMask), reads);
  let SchedRW = !listconcat(writes, allReads);
}

// Common class of scheduling definitions for n-ary instructions.
// The scheduling resources are relevant to LMUL and may be relevant to SEW.
class SchedNary<string write, list<string> reads, string mx, int sew = 0,
                bit forceMasked = 0, bit forcePassthruRead = 0>
    : SchedCommon<[!cast<SchedWrite>(
                       !if(sew,
                           write # "_" # mx # "_E" # sew,
                           write # "_" # mx))],
                  !foreach(read, reads,
                           !cast<SchedRead>(!if(sew,
                                                read # "_" # mx # "_E" #sew,
                                                read # "_" # mx))),
                  mx, sew, forceMasked, forcePassthruRead>;

// Classes with postfix "MC" are only used in MC layer.
// For these classes, we assume that they are with the worst case costs and
// `ReadVMask` is always needed (with some exceptions).

// For instructions with no operand.
class SchedNullary<string write, string mx, int sew = 0, bit forceMasked = 0,
                   bit forcePassthruRead = 0>:
  SchedNary<write, [], mx, sew, forceMasked, forcePassthruRead>;

// For instructions with one operand.
class SchedUnary<string write, string read0, string mx, int sew = 0,
                 bit forceMasked = 0, bit forcePassthruRead = 0>:
  SchedNary<write, [read0], mx, sew, forceMasked, forcePassthruRead>;
class SchedUnaryMC<string write, string read0, bit forceMasked = 1>:
  SchedUnary<write, read0, "WorstCase", forceMasked=forceMasked>;

// For instructions with two operands.
class SchedBinary<string write, string read0, string read1, string mx,
                  int sew = 0, bit forceMasked = 0, bit forcePassthruRead = 0>
    : SchedNary<write, [read0, read1], mx, sew, forceMasked, forcePassthruRead>;
class SchedBinaryMC<string write, string read0, string read1,
                    bit forceMasked = 1>:
  SchedBinary<write, read0, read1, "WorstCase", forceMasked=forceMasked>;

// For instructions with three operands.
class SchedTernary<string write, string read0, string read1, string read2,
                   string mx, int sew = 0, bit forceMasked = 0>
    : SchedNary<write, [read0, read1, read2], mx, sew, forceMasked>;
class SchedTernaryMC<string write, string read0, string read1, string read2,
                     int sew = 0, bit forceMasked = 1>:
  SchedNary<write, [read0, read1, read2], "WorstCase", sew, forceMasked>;

// For reduction instructions.
class SchedReduction<string write, string read, string mx, int sew,
                     bit forcePassthruRead = 0>
    : SchedCommon<[!cast<SchedWrite>(write #"_" #mx #"_E" #sew)],
                  !listsplat(!cast<SchedRead>(read), 3), mx, sew, forcePassthruRead>;

// Vector Unit-Stride Loads and Stores
class VLESched<string lmul, bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVLDE_" # lmul)],
  [ReadVLDX], mx=lmul, forceMasked=forceMasked
>;
class VLESchedMC : VLESched<"WorstCase", forceMasked=1>;

class VSESched<string lmul, bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVSTE_" # lmul)],
  [!cast<SchedRead>("ReadVSTEV_" # lmul), ReadVSTX], mx=lmul,
  forceMasked=forceMasked
>;

// Vector Strided Loads and Stores
class VLSSched<int eew, string emul, bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVLDS" # eew # "_" # emul)],
  [ReadVLDX, ReadVLDSX], emul, eew, forceMasked
>;

class VSSSched<int eew, string emul, bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVSTS" # eew # "_" # emul)],
  [!cast<SchedRead>("ReadVSTS" # eew # "V_" # emul), ReadVSTX, ReadVSTSX],
  emul, eew, forceMasked
>;

// Vector Indexed Loads and Stores
class VLXSched<int dataEEW, bit isOrdered, string dataEMUL, string idxEMUL,
               bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVLD" # !if(isOrdered, "O", "U") # "X" # dataEEW # "_" # dataEMUL)],
  [ReadVLDX, !cast<SchedRead>("ReadVLD" # !if(isOrdered, "O", "U") # "XV_" # idxEMUL)],
  dataEMUL, dataEEW, forceMasked
>;

class VSXSched<int dataEEW, bit isOrdered, string dataEMUL, string idxEMUL,
               bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVST" # !if(isOrdered, "O", "U") # "X" # dataEEW # "_" # dataEMUL)],
  [!cast<SchedRead>("ReadVST" # !if(isOrdered, "O", "U") #"X" # dataEEW # "_" # dataEMUL),
   ReadVSTX, !cast<SchedRead>("ReadVST" # !if(isOrdered, "O", "U") # "XV_" # idxEMUL)],
  dataEMUL, dataEEW, forceMasked
>;

// Unit-stride Fault-Only-First Loads
class VLFSched<string lmul, bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVLDFF_" # lmul)],
  [ReadVLDX], mx=lmul, forceMasked=forceMasked
>;

// Unit-Stride Segment Loads and Stores
class VLSEGSched<int nfields, int eew, string emul,bit forceMasked = 0>
    : SchedCommon<[!cast<SchedWrite>("WriteVLSEG" # nfields #"e" #eew #"_" #emul)],
                  [ReadVLDX], emul, eew, forceMasked
>;
class VLSEGSchedMC<int nfields, int eew>
    : VLSEGSched<nfields, eew, "WorstCase", forceMasked=1>;

class VSSEGSched<int nfields, int eew, string emul, bit forceMasked = 0>
    : SchedCommon<[!cast<SchedWrite>("WriteVSSEG" # nfields # "e" # eew # "_" # emul)],
                  [!cast<SchedRead>("ReadVSTEV_" #emul), ReadVSTX], emul, eew,
                  forceMasked>;
class VSSEGSchedMC<int nfields, int eew>
    : VSSEGSched<nfields, eew, "WorstCase", forceMasked=1>;

class VLSEGFFSched<int nfields, int eew, string emul, bit forceMasked = 0>
    : SchedCommon<[!cast<SchedWrite>("WriteVLSEGFF" # nfields # "e" # eew # "_" # emul)],
                  [ReadVLDX], emul, eew, forceMasked>;
class VLSEGFFSchedMC<int nfields, int eew>
    : VLSEGFFSched<nfields, eew, "WorstCase", forceMasked=1>;

// Strided Segment Loads and Stores
class VLSSEGSched<int nfields, int eew, string emul, bit forceMasked = 0>
    : SchedCommon<[!cast<SchedWrite>("WriteVLSSEG" # nfields #"e" #eew #"_" #emul)],
                  [ReadVLDX, ReadVLDSX], emul, eew, forceMasked>;
class VLSSEGSchedMC<int nfields, int eew>
    : VLSSEGSched<nfields, eew, "WorstCase", forceMasked=1>;

class VSSSEGSched<int nfields, int eew, string emul, bit forceMasked = 0>
    : SchedCommon<[!cast<SchedWrite>("WriteVSSSEG" # nfields #"e" #eew #"_" #emul)],
                  [!cast<SchedRead>("ReadVSTS" #eew #"V_" #emul), ReadVSTX, ReadVSTSX],
                  emul, eew, forceMasked>;
class VSSSEGSchedMC<int nfields, int eew>
    : VSSSEGSched<nfields, eew, "WorstCase", forceMasked=1>;

// Indexed Segment Loads and Stores
class VLXSEGSched<int nfields, int eew, bit isOrdered, string emul,
                  bit forceMasked = 0>
    : SchedCommon<[!cast<SchedWrite>("WriteVL" #!if(isOrdered, "O", "U") #"XSEG" # nfields #"e" #eew #"_" #emul)],
                  [ReadVLDX, !cast<SchedRead>("ReadVLD" #!if(isOrdered, "O", "U") #"XV_" #emul)],
                  emul, eew, forceMasked>;
class VLXSEGSchedMC<int nfields, int eew, bit isOrdered>
    : VLXSEGSched<nfields, eew, isOrdered, "WorstCase", forceMasked=1>;

// Passes sew=0 instead of eew=0 since this pseudo does not follow MX_E form.
class VSXSEGSched<int nfields, int eew, bit isOrdered, string emul,
                  bit forceMasked = 0> : SchedCommon<
  [!cast<SchedWrite>("WriteVS" #!if(isOrdered, "O", "U") #"XSEG" #nfields #"e" #eew #"_" #emul)],
  [!cast<SchedRead>("ReadVST" #!if(isOrdered, "O", "U") #"X" #eew #"_" #emul),
   ReadVSTX, !cast<SchedRead>("ReadVST" #!if(isOrdered, "O", "U") #"XV_" #emul)],
  emul, sew=0, forceMasked=forceMasked
>;
class VSXSEGSchedMC<int nfields, int eew, bit isOrdered>:
  VSXSEGSched<nfields, eew, isOrdered, "WorstCase", forceMasked=1>;

class RVTrimVXMemOpMC<int EEW, bit Ordered, bit Store, bits<4> N = 0> {
  bits<3> Log2EEW = !logtwo(EEW);
  bits<1> IsOrdered = Ordered;
  bits<1> IsStore = Store;
  bits<4> NFields = N;
  Instruction BaseInstr = !cast<Instruction>(NAME);
}

def RVTrimBaseVXMemOpTable : GenericTable {
  let FilterClass = "RVTrimVXMemOpMC";
  let CppTypeName = "VXMemOpInfo";
  let Fields = ["Log2EEW", "IsOrdered", "IsStore", "NFields", "BaseInstr"];
  let PrimaryKey = ["BaseInstr"];
  let PrimaryKeyName = "getVXMemOpInfo";
}

//===----------------------------------------------------------------------===//
// Instruction class templates
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 1, mayStore = 0 in {

// unit-stride segment load vd, (rs1), vm
class VUnitStrideSegmentLoad<int nfields, RVTrimWidth width, string opcodestr>
    : RVInstVLU<!sub(nfields, 1), width, LUMOPUnitStride, (outs VR:$vd),
                (ins GPRMemZeroOffset:$rs1, VMaskOp:$vm), opcodestr,
                "$vd, $rs1$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}

// segment fault-only-first load vd, (rs1), vm
class VUnitStrideSegmentLoadFF<int nfields, RVTrimWidth width, string opcodestr>
    : RVInstVLU<!sub(nfields, 1), width, LUMOPUnitStrideFF, (outs VR:$vd),
                (ins GPRMemZeroOffset:$rs1, VMaskOp:$vm), opcodestr,
                "$vd, $rs1$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}

// strided segment load vd, (rs1), rs2, vm
class VStridedSegmentLoad<int nfields, RVTrimWidth width, string opcodestr>
    : RVInstVLS<!sub(nfields, 1), width, (outs VR:$vd),
                (ins GPRMemZeroOffset:$rs1, GPR:$rs2, VMaskOp:$vm), opcodestr,
                "$vd, $rs1, $rs2$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}

// indexed segment load vd, (rs1), vs2, vm
class VIndexedSegmentLoad<int nfields, RVTrimMOP mop, RVTrimWidth width,
                          string opcodestr>
    : RVInstVLX<!sub(nfields, 1), width, mop, (outs VR:$vd),
                (ins GPRMemZeroOffset:$rs1, VR:$vs2, VMaskOp:$vm), opcodestr,
                "$vd, $rs1, $vs2$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}
} // hasSideEffects = 0, mayLoad = 1, mayStore = 0

let hasSideEffects = 0, mayLoad = 0, mayStore = 1 in {

// segment store vd, vs3, (rs1), vm
class VUnitStrideSegmentStore<int nfields, RVTrimWidth width, string opcodestr>
    : RVInstVSU<!sub(nfields, 1), width, SUMOPUnitStride, (outs),
                (ins VR:$vs3, GPRMemZeroOffset:$rs1, VMaskOp:$vm), opcodestr,
                "$vs3, $rs1$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}

// segment store vd, vs3, (rs1), rs2, vm
class VStridedSegmentStore<int nfields, RVTrimWidth width, string opcodestr>
    : RVInstVSS<!sub(nfields, 1), width, (outs),
                (ins VR:$vs3, GPRMemZeroOffset:$rs1, GPR:$rs2, VMaskOp:$vm),
                opcodestr, "$vs3, $rs1, $rs2$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}

// segment store vd, vs3, (rs1), vs2, vm
class VIndexedSegmentStore<int nfields, RVTrimMOP mop, RVTrimWidth width,
                           string opcodestr>
    : RVInstVSX<!sub(nfields, 1), width, mop, (outs),
                (ins VR:$vs3, GPRMemZeroOffset:$rs1, VR:$vs2, VMaskOp:$vm),
                opcodestr, "$vs3, $rs1, $vs2$vm"> {
  assert !and(!ge(nfields, 2), !le(nfields, 8)), "nfields must be 2-8";
}
} // hasSideEffects = 0, mayLoad = 0, mayStore = 1

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
// op vd, vs2, vs1, vm
class VALUVV<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVV<funct6, opv, (outs VR:$vd),
                (ins VR:$vs2, VR:$vs1, VMaskOp:$vm),
                opcodestr, "$vd, $vs2, $vs1$vm">;

// op vd, vs2, vs1, v0 (without mask, use v0 as carry input)
class VALUmVV<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVV<funct6, opv, (outs VR:$vd),
                (ins VR:$vs2, VR:$vs1, VMaskCarryInOp:$vm),
                opcodestr, "$vd, $vs2, $vs1, $vm"> {
  let vm = 0;
}

// op vd, vs1, vs2, vm (reverse the order of vs1 and vs2)
class VMACVV<bits<6> funct6, RVTrimVFormat opv, string opcodestr,
             bit EarlyClobber = 0>
    : RVInstVV<funct6, opv, (outs VR:$vd_wb),
                (ins VR:$vd, VR:$vs1, VR:$vs2, VMaskOp:$vm),
                opcodestr, "$vd, $vs1, $vs2$vm"> {
  let Constraints = !if(EarlyClobber, "@earlyclobber $vd_wb, $vd = $vd_wb",
                                      "$vd = $vd_wb");
}

// op vd, vs2, vs1
class VALUVVNoVm<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVV<funct6, opv, (outs VR:$vd),
               (ins VR:$vs2, VR:$vs1),
               opcodestr, "$vd, $vs2, $vs1"> {
  let vm = 1;

  let VMConstraint = false;
}

// op vd, vs2, rs1, vm
class VALUVX<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVX<funct6, opv, (outs VR:$vd),
                (ins VR:$vs2, GPR:$rs1, VMaskOp:$vm),
                opcodestr, "$vd, $vs2, $rs1$vm">;

// op vd, vs2, rs1, v0 (without mask, use v0 as carry input)
class VALUmVX<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVX<funct6, opv, (outs VR:$vd),
                (ins VR:$vs2, GPR:$rs1, VMaskCarryInOp:$vm),
                opcodestr, "$vd, $vs2, $rs1, $vm"> {
  let vm = 0;
}

// op vd, rs1, vs2, vm (reverse the order of rs1 and vs2)
class VMACVX<bits<6> funct6, RVTrimVFormat opv, string opcodestr,
             bit EarlyClobber = 0>
    : RVInstVX<funct6, opv, (outs VR:$vd_wb),
                (ins VR:$vd, GPR:$rs1, VR:$vs2, VMaskOp:$vm),
                opcodestr, "$vd, $rs1, $vs2$vm"> {
  let Constraints = !if(EarlyClobber, "@earlyclobber $vd_wb, $vd = $vd_wb",
                                      "$vd = $vd_wb");
}

// op vd, vs1, vs2
class VALUVXNoVm<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVX<funct6, opv, (outs VR:$vd),
               (ins VR:$vs2, GPR:$rs1),
               opcodestr, "$vd, $vs2, $rs1"> {
  let vm = 1;

  let VMConstraint = false;
}

// op vd, vs2, imm, vm
class VALUVI<bits<6> funct6, string opcodestr, Operand optype = simm5>
    : RVInstIVI<funct6, (outs VR:$vd),
                (ins VR:$vs2, optype:$imm, VMaskOp:$vm),
                opcodestr, "$vd, $vs2, $imm$vm">;

// op vd, vs2, imm
class VALUVINoVm<bits<6> funct6, string opcodestr, Operand optype = simm5>
    : RVInstIVI<funct6, (outs VR:$vd),
                (ins VR:$vs2, optype:$imm),
                opcodestr, "$vd, $vs2, $imm"> {
  let vm = 1;

  let VMConstraint = false;
}

// op vd, vs2, rs1, vm (Float)
class VALUVF<bits<6> funct6, RVTrimVFormat opv, string opcodestr>
    : RVInstVX<funct6, opv, (outs VR:$vd),
                (ins VR:$vs2, FPR32:$rs1, VMaskOp:$vm),
                opcodestr, "$vd, $vs2, $rs1$vm">;

// op vd, rs1, vs2, vm (Float) (with mask, reverse the order of rs1 and vs2)
class VMACVF<bits<6> funct6, RVTrimVFormat opv, string opcodestr,
             bit EarlyClobber = 0>
    : RVInstVX<funct6, opv, (outs VR:$vd_wb),
                (ins VR:$vd, FPR32:$rs1, VR:$vs2, VMaskOp:$vm),
                opcodestr, "$vd, $rs1, $vs2$vm"> {
  let Constraints = !if(EarlyClobber, "@earlyclobber $vd_wb, $vd = $vd_wb",
                                      "$vd = $vd_wb");
}

// op vd, vs2, vm (use vs1 as instruction encoding)
class VALUVs2<bits<6> funct6, bits<5> vs1, RVTrimVFormat opv, string opcodestr>
    : RVInstVUnary<funct6, vs1, opv, (outs VR:$vd),
                   (ins VR:$vs2, VMaskOp:$vm),
                   opcodestr, "$vd, $vs2$vm">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0

//===----------------------------------------------------------------------===//
// Combination of instruction classes.
// Use these multiclasses to define instructions more easily.
//===----------------------------------------------------------------------===//

multiclass VALU_IV_V<string opcodestr, bits<6> funct6> {
  def V  : VALUVV<funct6, OPIVV, opcodestr # ".vv">,
           SchedBinaryMC<"WriteVIALUV", "ReadVIALUV", "ReadVIALUV">;
}

multiclass VALU_IV_X<string opcodestr, bits<6> funct6> {
  def X  : VALUVX<funct6, OPIVX, opcodestr # ".vx">,
           SchedBinaryMC<"WriteVIALUX", "ReadVIALUV", "ReadVIALUX">;
}

multiclass VALU_IV_V_X<string opcodestr, bits<6> funct6>
    : VALU_IV_V<opcodestr, funct6>,
      VALU_IV_X<opcodestr, funct6>;

multiclass VWMAC_FV_V_F<string opcodestr, bits<6> funct6> {
  let VS1VS2Constraint = WidenV in {
  def V : VMACVV<funct6, OPFVV, opcodestr # ".vv", EarlyClobber=1>,
          SchedTernaryMC<"WriteVFWMulAddV", "ReadVFWMulAddV", "ReadVFWMulAddV",
                         "ReadVFWMulAddV">;
  def F : VMACVF<funct6, OPFVF, opcodestr # ".vf", EarlyClobber=1>,
          SchedTernaryMC<"WriteVFWMulAddF", "ReadVFWMulAddV", "ReadVFWMulAddF",
                         "ReadVFWMulAddV">;
  }
}

class VWCVTF_FV_VS2<string opcodestr, bits<6> funct6, bits<5> vs1>
    : VALUVs2<funct6, vs1, OPFVV, opcodestr>,
      SchedUnaryMC<"WriteVFWCvtFToFV", "ReadVFWCvtFToFV">;

class VNCVTF_FV_VS2<string opcodestr, bits<6> funct6, bits<5> vs1>
    : VALUVs2<funct6, vs1, OPFVV, opcodestr>,
      SchedUnaryMC<"WriteVFNCvtFToFV", "ReadVFNCvtFToFV">;

multiclass VSHT_IV_V_X_I<string opcodestr, bits<6> funct6> {
  def V  : VALUVV<funct6, OPIVV, opcodestr # ".vv">,
           SchedBinaryMC<"WriteVShiftV", "ReadVShiftV", "ReadVShiftV">;
  def X  : VALUVX<funct6, OPIVX, opcodestr # ".vx">,
           SchedBinaryMC<"WriteVShiftX", "ReadVShiftV", "ReadVShiftX">;
  def I  : VALUVI<funct6, opcodestr # ".vi", uimm5>,
           SchedUnaryMC<"WriteVShiftI", "ReadVShiftV">;
}

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

// vsetvli/vsetivli/vsetvl are generated from auto-td/opcodes.

let Predicates = [HasVInstructions] in {
def : MnemonicAlias<"vle1.v", "vlm.v">;
def : MnemonicAlias<"vse1.v", "vsm.v">;

def : InstAlias<"vl1r.v $vd, $rs1", (VL1RE8_V VR:$vd,   GPRMemZeroOffset:$rs1)>;
def : InstAlias<"vl2r.v $vd, $rs1", (VL2RE8_V VRM2:$vd, GPRMemZeroOffset:$rs1)>;
def : InstAlias<"vl4r.v $vd, $rs1", (VL4RE8_V VRM4:$vd, GPRMemZeroOffset:$rs1)>;
def : InstAlias<"vl8r.v $vd, $rs1", (VL8RE8_V VRM8:$vd, GPRMemZeroOffset:$rs1)>;
} // Predicates = [HasVInstructions]

let Predicates = [HasVInstructions] in {
// Vector Single-Width Integer Add and Subtract

def : InstAlias<"vneg.v $vd, $vs$vm", (VRSUB_VX VR:$vd, VR:$vs, X0, VMaskOp:$vm)>;
def : InstAlias<"vneg.v $vd, $vs", (VRSUB_VX VR:$vd, VR:$vs, X0, zero_reg)>;

def : InstAlias<"vwcvt.x.x.v $vd, $vs$vm",
                (VWADD_VX VR:$vd, VR:$vs, X0, VMaskOp:$vm)>;
def : InstAlias<"vwcvt.x.x.v $vd, $vs",
                (VWADD_VX VR:$vd, VR:$vs, X0, zero_reg)>;
def : InstAlias<"vwcvtu.x.x.v $vd, $vs$vm",
                (VWADDU_VX VR:$vd, VR:$vs, X0, VMaskOp:$vm)>;
def : InstAlias<"vwcvtu.x.x.v $vd, $vs",
                (VWADDU_VX VR:$vd, VR:$vs, X0, zero_reg)>;

def : InstAlias<"vnot.v $vd, $vs$vm",
                (VXOR_VI VR:$vd, VR:$vs, -1, VMaskOp:$vm)>;
def : InstAlias<"vnot.v $vd, $vs",
                (VXOR_VI VR:$vd, VR:$vs, -1, zero_reg)>;

def : InstAlias<"vncvt.x.x.w $vd, $vs$vm",
                (VNSRL_WX VR:$vd, VR:$vs, X0, VMaskOp:$vm)>;
def : InstAlias<"vncvt.x.x.w $vd, $vs",
                (VNSRL_WX VR:$vd, VR:$vs, X0, zero_reg)>;

def : InstAlias<"vmsgtu.vv $vd, $va, $vb$vm",
                (VMSLTU_VV VR:$vd, VR:$vb, VR:$va, VMaskOp:$vm), 0>;
def : InstAlias<"vmsgt.vv $vd, $va, $vb$vm",
                (VMSLT_VV VR:$vd, VR:$vb, VR:$va, VMaskOp:$vm), 0>;
def : InstAlias<"vmsgeu.vv $vd, $va, $vb$vm",
                (VMSLEU_VV VR:$vd, VR:$vb, VR:$va, VMaskOp:$vm), 0>;
def : InstAlias<"vmsge.vv $vd, $va, $vb$vm",
                (VMSLE_VV VR:$vd, VR:$vb, VR:$va, VMaskOp:$vm), 0>;

let ElementsDependOn = EltDepsNone in {

let isCodeGenOnly = 0, hasSideEffects = 0, mayLoad = 0,
    mayStore = 0, DestEEW = EEW1 in {
// For unsigned comparisons we need to special case 0 immediate to maintain
// the always true/false semantics we would invert if we just decremented the
// immediate like we do for signed. To match the GNU assembler we will use
// vmseq/vmsne.vv with the same register for both operands which we can't do
// from an InstAlias.
def PseudoVMSGEU_VI : Pseudo<(outs VR:$vd),
                             (ins VR:$vs2, simm5_plus1:$imm, VMaskOp:$vm),
                             [], "vmsgeu.vi", "$vd, $vs2, $imm$vm">;
def PseudoVMSLTU_VI : Pseudo<(outs VR:$vd),
                             (ins VR:$vs2, simm5_plus1:$imm, VMaskOp:$vm),
                             [], "vmsltu.vi", "$vd, $vs2, $imm$vm">;
// Handle signed with pseudos as well for more consistency in the
// implementation.
def PseudoVMSGE_VI : Pseudo<(outs VR:$vd),
                            (ins VR:$vs2, simm5_plus1:$imm, VMaskOp:$vm),
                            [], "vmsge.vi", "$vd, $vs2, $imm$vm">;
def PseudoVMSLT_VI : Pseudo<(outs VR:$vd),
                            (ins VR:$vs2, simm5_plus1:$imm, VMaskOp:$vm),
                            [], "vmslt.vi", "$vd, $vs2, $imm$vm">;
}

let isCodeGenOnly = 0, hasSideEffects = 0, mayLoad = 0,
    mayStore = 0, DestEEW = EEW1 in {
def PseudoVMSGEU_VX_M : Pseudo<(outs VR:$vd),
                               (ins VR:$vs2, GPR:$rs1, VMaskOp:$vm),
                               [], "vmsgeu.vx", "$vd, $vs2, $rs1$vm">;
def PseudoVMSGE_VX_M : Pseudo<(outs VR:$vd),
                              (ins VR:$vs2, GPR:$rs1, VMaskOp:$vm),
                              [], "vmsge.vx", "$vd, $vs2, $rs1$vm">;
def PseudoVMSGEU_VX_M_T : Pseudo<(outs VR:$vd, VRNoV0:$scratch),
                                 (ins VR:$vs2, GPR:$rs1, VMaskOp:$vm),
                                 [], "vmsgeu.vx", "$vd, $vs2, $rs1$vm, $scratch">;
def PseudoVMSGE_VX_M_T : Pseudo<(outs VR:$vd, VRNoV0:$scratch),
                                (ins VR:$vs2, GPR:$rs1, VMaskOp:$vm),
                                [], "vmsge.vx", "$vd, $vs2, $rs1$vm, $scratch">;
}

} // ElementsDependOn = EltDepsNone

} // Predicates = [HasVInstructions]

let Predicates = [HasVInstructionsAnyF] in {

def : InstAlias<"vfneg.v $vd, $vs$vm",
                (VFSGNJN_VV VR:$vd, VR:$vs, VR:$vs, VMaskOp:$vm)>;
def : InstAlias<"vfneg.v $vd, $vs",
                (VFSGNJN_VV VR:$vd, VR:$vs, VR:$vs, zero_reg)>;
def : InstAlias<"vfabs.v $vd, $vs$vm",
                (VFSGNJX_VV VR:$vd, VR:$vs, VR:$vs, VMaskOp:$vm)>;
def : InstAlias<"vfabs.v $vd, $vs",
                (VFSGNJX_VV VR:$vd, VR:$vs, VR:$vs, zero_reg)>;

def : InstAlias<"vmfgt.vv $vd, $va, $vb$vm",
                (VMFLT_VV VR:$vd, VR:$vb, VR:$va, VMaskOp:$vm), 0>;
def : InstAlias<"vmfge.vv $vd, $va, $vb$vm",
                (VMFLE_VV VR:$vd, VR:$vb, VR:$va, VMaskOp:$vm), 0>;

} // Predicates = HasVInstructionsAnyF]

let Predicates = [HasVInstructionsAnyF] in {

def : MnemonicAlias<"vfredsum.vs", "vfredusum.vs">;

def : MnemonicAlias<"vfwredsum.vs", "vfwredusum.vs">;
} // Predicates = [HasVInstructionsAnyF]

let Predicates = [HasVInstructions] in {

def : InstAlias<"vmmv.m $vd, $vs",
                (VMAND_MM VR:$vd, VR:$vs, VR:$vs)>;
def : InstAlias<"vmclr.m $vd",
                (VMXOR_MM VR:$vd, VR:$vd, VR:$vd)>;
def : InstAlias<"vmset.m $vd",
                (VMXNOR_MM VR:$vd, VR:$vd, VR:$vd)>;
def : InstAlias<"vmnot.m $vd, $vs",
                (VMNAND_MM VR:$vd, VR:$vs, VR:$vs)>;

def : MnemonicAlias<"vmandnot.mm", "vmandn.mm">;
def : MnemonicAlias<"vmornot.mm", "vmorn.mm">;

def : MnemonicAlias<"vpopc.m", "vcpop.m">;

} // Predicates = [HasVInstructions]

let ElementsDependOn = EltDepsNone in {
let Predicates = [HasVInstructions] in {
  foreach nfields=2-8 in {
    foreach eew = [8, 16, 32] in {
      defvar w = !cast<RVTrimWidth>("LSWidth"#eew);

      def VLSEG#nfields#E#eew#_V :
        VUnitStrideSegmentLoad<nfields, w, "vlseg"#nfields#"e"#eew#".v">,
        VLSEGSchedMC<nfields, eew>;
      def VLSEG#nfields#E#eew#FF_V :
        VUnitStrideSegmentLoadFF<nfields, w, "vlseg"#nfields#"e"#eew#"ff.v">,
        VLSEGFFSchedMC<nfields, eew>;
      def VSSEG#nfields#E#eew#_V :
        VUnitStrideSegmentStore<nfields, w, "vsseg"#nfields#"e"#eew#".v">,
        VSSEGSchedMC<nfields, eew>;
      // Vector Strided Instructions
      def VLSSEG#nfields#E#eew#_V :
        VStridedSegmentLoad<nfields, w, "vlsseg"#nfields#"e"#eew#".v">,
        VLSSEGSchedMC<nfields, eew>;
      def VSSSEG#nfields#E#eew#_V :
        VStridedSegmentStore<nfields, w, "vssseg"#nfields#"e"#eew#".v">,
        VSSSEGSchedMC<nfields, eew>;

      // Vector Indexed Instructions
      let VS1VS2Constraint = VS2Constraint in {
      def VLUXSEG#nfields#EI#eew#_V :
        VIndexedSegmentLoad<nfields, MOPLDIndexedUnord, w,
                            "vluxseg"#nfields#"ei"#eew#".v">,
        RVTrimVXMemOpMC<eew, Ordered=false, Store=false, N=nfields>,
        VLXSEGSchedMC<nfields, eew, isOrdered=0>;
      def VLOXSEG#nfields#EI#eew#_V :
        VIndexedSegmentLoad<nfields, MOPLDIndexedOrder, w,
                            "vloxseg"#nfields#"ei"#eew#".v">,
        RVTrimVXMemOpMC<eew, Ordered=true, Store=false, N=nfields>,
        VLXSEGSchedMC<nfields, eew, isOrdered=1>;
      } // VS1VS2Constraint = VS2Constraint
      def VSUXSEG#nfields#EI#eew#_V :
        VIndexedSegmentStore<nfields, MOPSTIndexedUnord, w,
                             "vsuxseg"#nfields#"ei"#eew#".v">,
        RVTrimVXMemOpMC<eew, Ordered=false, Store=true, N=nfields>,
        VSXSEGSchedMC<nfields, eew, isOrdered=0>;
      def VSOXSEG#nfields#EI#eew#_V :
        VIndexedSegmentStore<nfields, MOPSTIndexedOrder, w,
                             "vsoxseg"#nfields#"ei"#eew#".v">,
        RVTrimVXMemOpMC<eew, Ordered=true, Store=true, N=nfields>,
        VSXSEGSchedMC<nfields, eew, isOrdered=1>;
    }
  }
} // Predicates = [HasVInstructions]

let Predicates = [HasVInstructionsI64] in {
  foreach nfields=2-8 in {
    // Vector Unit-strided Segment Instructions
    def VLSEG#nfields#E64_V :
      VUnitStrideSegmentLoad<nfields, LSWidth64, "vlseg"#nfields#"e64.v">,
      VLSEGSchedMC<nfields, 64>;
    def VLSEG#nfields#E64FF_V :
      VUnitStrideSegmentLoadFF<nfields, LSWidth64, "vlseg"#nfields#"e64ff.v">,
      VLSEGFFSchedMC<nfields, 64>;
    def VSSEG#nfields#E64_V :
      VUnitStrideSegmentStore<nfields, LSWidth64, "vsseg"#nfields#"e64.v">,
      VSSEGSchedMC<nfields, 64>;

    // Vector Strided Segment Instructions
    def VLSSEG#nfields#E64_V :
      VStridedSegmentLoad<nfields, LSWidth64, "vlsseg"#nfields#"e64.v">,
      VLSSEGSchedMC<nfields, 64>;
    def VSSSEG#nfields#E64_V :
      VStridedSegmentStore<nfields, LSWidth64, "vssseg"#nfields#"e64.v">,
      VSSSEGSchedMC<nfields, 64>;
  }
} // Predicates = [HasVInstructionsI64]
let Predicates = [HasVInstructionsI64, IsRV64] in {
  foreach nfields = 2 - 8 in {
    // Vector Indexed Segment Instructions
    let VS1VS2Constraint = VS2Constraint in {
    def VLUXSEG #nfields #EI64_V
        : VIndexedSegmentLoad<nfields, MOPLDIndexedUnord, LSWidth64,
                              "vluxseg" #nfields #"ei64.v">,
          VLXSEGSchedMC<nfields, 64, isOrdered=0>;
    def VLOXSEG #nfields #EI64_V
        : VIndexedSegmentLoad<nfields, MOPLDIndexedOrder, LSWidth64,
                              "vloxseg" #nfields #"ei64.v">,
          VLXSEGSchedMC<nfields, 64, isOrdered=1>;
    } // VS1VS2Constraint = VS2Constraint
    def VSUXSEG #nfields #EI64_V
        : VIndexedSegmentStore<nfields, MOPSTIndexedUnord, LSWidth64,
                               "vsuxseg" #nfields #"ei64.v">,
          VSXSEGSchedMC<nfields, 64, isOrdered=0>;
    def VSOXSEG #nfields #EI64_V
        : VIndexedSegmentStore<nfields, MOPSTIndexedOrder, LSWidth64,
                               "vsoxseg" #nfields #"ei64.v">,
          VSXSEGSchedMC<nfields, 64, isOrdered=1>;
  }
} // Predicates = [HasVInstructionsI64, IsRV64]
} // ElementsDependOn = EltDepsNone

include "RVTrimInstrInfoVPseudos.td"
include "RVTrimInstrInfoZvfbf.td"
// Include the non-intrinsic ISel patterns
include "RVTrimInstrInfoVVLPatterns.td"
include "RVTrimInstrInfoVSDPatterns.td"
