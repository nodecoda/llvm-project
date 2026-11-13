//===-- RVTrimInstrInfoVOperands.td - RISC-V 'V' operands ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// Operand and SDNode transformation definitions for the standard 'V' vector
/// extension.
///
/// This file must be included before auto-td/AutoTdInstrInfo.inc: generated V
/// records reference VMaskOp, VMaskCarryInOp, VTypeIOp* and simm5, and
/// TableGen requires every class to be defined before it is used.
///
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Operand and SDNode transformation definitions.
//===----------------------------------------------------------------------===//

class VTypeIAsmOperand<int VTypeINum> : AsmOperandClass {
  let Name = "VTypeI" # VTypeINum;
  let ParserMethod = "parseVTypeI";
  let DiagnosticType = "InvalidVTypeI";
  let RenderMethod = "addVTypeIOperands";
}

class VTypeIOp<int VTypeINum> : RVTrimOp {
  let ParserMatchClass = VTypeIAsmOperand<VTypeINum>;
  let PrintMethod = "printVTypeI";
  let DecoderMethod = "decodeUImmOperand<"#VTypeINum#">";
  let OperandType = "OPERAND_VTYPEI" # VTypeINum;
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isUInt<}] # VTypeINum # [{>(Imm);
    return MCOp.Kind == KindTy::VType;
  }];
}

def VTypeIOp10 : VTypeIOp<10>;
def VTypeIOp11 : VTypeIOp<11>;

def VMaskAsmOperand : AsmOperandClass {
  let Name = "RVVMaskRegOpOperand";
  let RenderMethod = "addRegOperands";
  let PredicateMethod = "isV0Reg";
  let ParserMethod = "parseMaskReg";
  let IsOptional = 1;
  let DefaultMethod = "defaultMaskRegOp";
  let DiagnosticType = "InvalidVMaskRegister";
  let DiagnosticString = "operand must be v0.t";
}

def VMaskCarryInAsmOperand : AsmOperandClass {
  let Name = "RVVMaskCarryInRegOpOperand";
  let RenderMethod = "addRegOperands";
  let PredicateMethod = "isV0Reg";
  let DiagnosticType = "InvalidVMaskCarryInRegister";
  let DiagnosticString = "operand must be v0";
}

// An optional v0.t operand encoded in vm.
def VMaskOp : RegisterOperand<VMV0> {
  let ParserMatchClass = VMaskAsmOperand;
  let PrintMethod = "printVMaskReg";
  let EncoderMethod = "getVMaskReg";
  let DecoderMethod = "decodeVMaskReg";
  let OperandNamespace = "RVTrimOp";
  let OperandType = "OPERAND_VMASK";
}

// An always present v0 operand encoded with vm=0. Classes that use this must
// set the vm field in RVInstV* to 0.
def VMaskCarryInOp : RegisterOperand<VMV0> {
  let ParserMatchClass = VMaskCarryInAsmOperand;
  let EncoderMethod = "getVMaskReg";
  let DecoderMethod = "decodeVMaskReg";
}

def simm5 : RVTrimSImmLeafOp<5> {
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return isInt<5>(Imm);
    return MCOp.isBareSymbolRef();
  }];
}

def simm5_plus1 : RVTrimOp, ImmLeaf<XLenVT,
  [{return Imm >= -15 && Imm <= 16;}]> {
  let ParserMatchClass = SImmAsmOperand<5, "Plus1">;
  let OperandType = "OPERAND_SIMM5_PLUS1";
  let MCOperandPredicate = [{
    int64_t Imm;
    if (MCOp.evaluateAsConstantImm(Imm))
      return Imm >= -15 && Imm <= 16;
    return MCOp.isBareSymbolRef();
  }];
}

def simm5_plus1_nonzero : ImmLeaf<XLenVT,
  [{return Imm != 0 && Imm >= -15 && Imm <= 16;}]>;
