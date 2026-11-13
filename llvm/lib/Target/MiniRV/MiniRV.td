//===-- MiniRV.td - Describe the MiniRV demo target --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MiniRV is an MC-only target.  Its purpose is to show that a second target
// can be brought up from an auto-td corpus: the instruction records, their
// encodings, the asm matcher, the printer and the decoder all come from the
// generated file plus this small amount of hand-written TableGen.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

include "MiniRVRegisterInfo.td"

// Map the pointer operands of the standard pseudo instructions
// (LOAD_STACK_GUARD, ...) onto the only register class we have.
defm : RemapAllTargetPseudoPointerOperands<GPR>;

class Proc<string Name, list<SubtargetFeature> Features>
    : Processor<Name, NoItineraries, Features>;
def : Proc<"generic", []>;

include "MiniRVCallingConv.td"

include "MiniRVInstrInfo.td"

def MiniRVInstrInfo : InstrInfo;

def MiniRVAsmWriter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
}

def MiniRVAsmParser : AsmParser;

def MiniRV : Target {
  let InstructionSet = MiniRVInstrInfo;
  let AssemblyParsers = [MiniRVAsmParser];
}
