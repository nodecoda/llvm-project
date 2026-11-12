//===-- RVTrimInstrFormatsV.td - RISC-V V Instruction Formats -*- tablegen -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file describes the RISC-V V extension instruction formats.
//
//===----------------------------------------------------------------------===//

class RVTrimVFormat<bits<3> val> {
  bits<3> Value = val;
}
def OPIVV : RVTrimVFormat<0b000>;
def OPFVV : RVTrimVFormat<0b001>;
def OPMVV : RVTrimVFormat<0b010>;
def OPIVI : RVTrimVFormat<0b011>;
def OPIVX : RVTrimVFormat<0b100>;
def OPFVF : RVTrimVFormat<0b101>;
def OPMVX : RVTrimVFormat<0b110>;
def OPCFG : RVTrimVFormat<0b111>;

class RVTrimMOP<bits<2> val> {
  bits<2> Value = val;
}
def MOPLDUnitStride   : RVTrimMOP<0b00>;
def MOPLDIndexedUnord : RVTrimMOP<0b01>;
def MOPLDStrided      : RVTrimMOP<0b10>;
def MOPLDIndexedOrder : RVTrimMOP<0b11>;

def MOPSTUnitStride   : RVTrimMOP<0b00>;
def MOPSTIndexedUnord : RVTrimMOP<0b01>;
def MOPSTStrided      : RVTrimMOP<0b10>;
def MOPSTIndexedOrder : RVTrimMOP<0b11>;

class RVTrimLUMOP<bits<5> val> {
  bits<5> Value = val;
}
def LUMOPUnitStride  : RVTrimLUMOP<0b00000>;
def LUMOPUnitStrideMask : RVTrimLUMOP<0b01011>;
def LUMOPUnitStrideWholeReg : RVTrimLUMOP<0b01000>;
def LUMOPUnitStrideFF: RVTrimLUMOP<0b10000>;

class RVTrimSUMOP<bits<5> val> {
  bits<5> Value = val;
}
def SUMOPUnitStride  : RVTrimSUMOP<0b00000>;
def SUMOPUnitStrideMask : RVTrimSUMOP<0b01011>;
def SUMOPUnitStrideWholeReg : RVTrimSUMOP<0b01000>;

class RVTrimWidth<bits<4> val> {
  bits<4> Value = val;
}
def LSWidth8     : RVTrimWidth<0b0000>;
def LSWidth16    : RVTrimWidth<0b0101>;
def LSWidth32    : RVTrimWidth<0b0110>;
def LSWidth64    : RVTrimWidth<0b0111>;

class RVInstVSetiVLi<dag outs, dag ins, string opcodestr, string argstr>
    : RVInstIBase<OPCFG.Value, OPC_OP_V, outs, ins, opcodestr, argstr> {
  bits<5> uimm;
  bits<10> vtypei;

  let rs1 = uimm;

  let Inst{31} = 1;
  let Inst{30} = 1;
  let Inst{29-20} = vtypei;

  let Defs = [VL, VTYPE];
}

class RVInstVSetVLi<dag outs, dag ins, string opcodestr, string argstr>
    : RVInstIBase<OPCFG.Value, OPC_OP_V, outs, ins, opcodestr, argstr> {
  bits<11> vtypei;

  let Inst{31} = 0;
  let Inst{30-20} = vtypei;

  let Defs = [VL, VTYPE];
}

class RVInstVSetVL<dag outs, dag ins, string opcodestr, string argstr>
    : RVInstR<0b1000000, OPCFG.Value, OPC_OP_V, outs, ins, opcodestr, argstr> {
  let Defs = [VL, VTYPE];
}

class RVInstVBase<bits<6> funct6, RVTrimVFormat opv, dag outs, dag ins,
                  string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatR> {
  bits<5> vd;
  bit vm;

  let Inst{31-26} = funct6;
  let Inst{25} = vm;
  // Inst{24-15} provide by subclasses
  let Inst{14-12} = opv.Value;
  let Inst{11-7} = vd;
  let Inst{6-0} = OPC_OP_V.Value;

  let Uses = [VL, VTYPE];
  let VMConstraint = true;

  let UseNamedOperandTable = true;
}

class RVInstVV<bits<6> funct6, RVTrimVFormat opv, dag outs, dag ins,
               string opcodestr, string argstr>
    : RVInstVBase<funct6, opv, outs, ins, opcodestr, argstr> {
  bits<5> vs2;
  bits<5> vs1;

  let Inst{24-20} = vs2;
  let Inst{19-15} = vs1;
}

class RVInstVX<bits<6> funct6, RVTrimVFormat opv, dag outs, dag ins,
                string opcodestr, string argstr>
    : RVInstVBase<funct6, opv, outs, ins, opcodestr, argstr> {
  bits<5> vs2;
  bits<5> rs1;

  let Inst{24-20} = vs2;
  let Inst{19-15} = rs1;
}

class RVInstIVI<bits<6> funct6, dag outs, dag ins, string opcodestr,
                string argstr>
    : RVInstVBase<funct6, OPIVI, outs, ins, opcodestr, argstr> {
  bits<5> vs2;
  bits<5> imm;

  let Inst{24-20} = vs2;
  let Inst{19-15} = imm;
}

class RVInstVUnary<bits<6> funct6, bits<5> vs1, RVTrimVFormat opv, dag outs,
                   dag ins, string opcodestr, string argstr>
    : RVInstVBase<funct6, opv, outs, ins, opcodestr, argstr> {
  bits<5> vs2;

  let Inst{24-20} = vs2;
  let Inst{19-15} = vs1;
}

class RVInstVUnaryRd<bits<6> funct6, bits<5> vs1, RVTrimVFormat opv, dag outs,
                     dag ins, string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatR> {
  bits<5> rd;
  bit vm;
  bits<5> vs2;

  let Inst{31-26} = funct6;
  let Inst{25} = vm;
  let Inst{24-20} = vs2;
  let Inst{19-15} = vs1;
  let Inst{14-12} = opv.Value;
  let Inst{11-7} = rd;
  let Inst{6-0} = OPC_OP_V.Value;

  let Uses = [VL, VTYPE];

  let UseNamedOperandTable = true;
}

class RVInstVLoadBase<bits<3> nf, RVTrimWidth width, RVTrimMOP mop,
                      dag outs, dag ins, string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatR> {
  bits<5> rs1;
  bits<5> vd;
  bit vm;

  let Inst{31-29} = nf;
  let Inst{28} = width.Value{3};
  let Inst{27-26} = mop.Value;
  let Inst{25} = vm;
  // Inst{24-20} provided by subclasses
  let Inst{19-15} = rs1;
  let Inst{14-12} = width.Value{2-0};
  let Inst{11-7} = vd;
  let Inst{6-0} = OPC_LOAD_FP.Value;

  let Uses = [VL, VTYPE];
  let VMConstraint = true;

  let UseNamedOperandTable = true;
}

class RVInstVLU<bits<3> nf, RVTrimWidth width, RVTrimLUMOP lumop, dag outs,
                dag ins, string opcodestr, string argstr>
    : RVInstVLoadBase<nf, width, MOPLDUnitStride, outs, ins, opcodestr,
                      argstr> {
  let Inst{24-20} = lumop.Value;
}

class RVInstVLS<bits<3> nf, RVTrimWidth width, dag outs, dag ins,
                string opcodestr, string argstr>
    : RVInstVLoadBase<nf, width, MOPLDStrided, outs, ins, opcodestr, argstr> {
  bits<5> rs2;

  let Inst{24-20} = rs2;
}

class RVInstVLX<bits<3> nf, RVTrimWidth width, RVTrimMOP mop, dag outs, dag ins,
                string opcodestr, string argstr>
    : RVInstVLoadBase<nf, width, mop, outs, ins, opcodestr, argstr> {
  bits<5> vs2;

  let Inst{24-20} = vs2;
}

class RVInstVStoreBase<bits<3> nf, RVTrimWidth width, RVTrimMOP mop, dag outs,
                       dag ins, string opcodestr, string argstr>
    : RVInst<outs, ins, opcodestr, argstr, [], InstFormatR> {
  bits<5> rs1;
  bits<5> vs3;
  bit vm;

  let Inst{31-29} = nf;
  let Inst{28} = width.Value{3};
  let Inst{27-26} = mop.Value;
  let Inst{25} = vm;
  // Inst{24-20} provided by subclasses
  let Inst{19-15} = rs1;
  let Inst{14-12} = width.Value{2-0};
  let Inst{11-7} = vs3;
  let Inst{6-0} = OPC_STORE_FP.Value;

  let Uses = [VL, VTYPE];

  let UseNamedOperandTable = true;
}

class RVInstVSU<bits<3> nf, RVTrimWidth width, RVTrimSUMOP sumop, dag outs,
                dag ins, string opcodestr, string argstr>
    : RVInstVStoreBase<nf, width, MOPSTUnitStride, outs, ins, opcodestr,
                       argstr> {
  let Inst{24-20} = sumop.Value;
}

class RVInstVSS<bits<3> nf, RVTrimWidth width, dag outs, dag ins,
                string opcodestr, string argstr>
    : RVInstVStoreBase<nf, width, MOPSTStrided, outs, ins, opcodestr,
                       argstr> {
  bits<5> rs2;

  let Inst{24-20} = rs2;
}

class RVInstVSX<bits<3> nf, RVTrimWidth width, RVTrimMOP mop, dag outs, dag ins,
                string opcodestr, string argstr>
    : RVInstVStoreBase<nf, width, mop, outs, ins, opcodestr, argstr> {
  bits<5> vs2;

  let Inst{24-20} = vs2;
}
