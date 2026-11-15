//===-- RVMin.td - Minimal RV32I backend -------------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level TableGen entry for the RVMin target: a from-scratch backend that
// implements the RV32I base integer instruction set.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Features
//===----------------------------------------------------------------------===//
//
// Chapter 13: the M extension is optional.  Without `-mattr=+m` the backend is
// exactly the RV32I machine of chapters 1-12; with it, multiply, divide and
// remainder become single instructions instead of libcalls.

def FeatureStdExtM : SubtargetFeature<"m", "HasStdExtM", "true",
                                      "Integer multiplication and division">;

def HasStdExtM : Predicate<"Subtarget->hasStdExtM()">,
                 AssemblerPredicate<(all_of FeatureStdExtM),
                                    "'M' (Integer Multiplication and Division)">;

//===----------------------------------------------------------------------===//
// Processors
//===----------------------------------------------------------------------===//

class RVMinProc<string Name, list<SubtargetFeature> Features>
    : Processor<Name, NoItineraries, Features>;

def : RVMinProc<"generic-rv32", []>;
def : RVMinProc<"generic-rv32-m", [FeatureStdExtM]>;

//===----------------------------------------------------------------------===//
// Register File
//===----------------------------------------------------------------------===//

include "RVMinRegisterInfo.td"

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

include "RVMinInstrInfo.td"

include "RVMinCallingConv.td"

// Remap pointer operands of the target-independent pseudo instructions
// (PREALLOCATED_ARG, LOAD_STACK_GUARD, ...) onto the RVMin GPR class.
defm : RemapAllTargetPseudoPointerOperands<GPR>;

def RVMinInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Assembly parser / writer
//===----------------------------------------------------------------------===//

// The writer is implemented by hand in MCTargetDesc/RVMinInstPrinter.cpp on top
// of the generated per-instruction printing code.
def RVMinAsmWriter : AsmWriter {
  let AsmWriterClassName = "InstPrinter";
  int PassSubtarget = 1;
}

// The parser is hand written; TableGen generates the matcher tables from the
// instruction AsmStrings and operand match classes.
def RVMinAsmParser : AsmParser {
  let ShouldEmitMatchRegisterAltName = true;
  let AllowDuplicateRegisterNames = true;
}

//===----------------------------------------------------------------------===//
// Target Declaration
//===----------------------------------------------------------------------===//

def RVMin : Target {
  let InstructionSet = RVMinInstrInfo;
  let AssemblyParsers = [RVMinAsmParser];
  let AssemblyWriters = [RVMinAsmWriter];
}
