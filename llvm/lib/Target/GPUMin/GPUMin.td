//===-- GPUMin.td - Minimal SIMT GPU backend ---------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level TableGen entry for the GPUMin target: a from-scratch backend for
// GM32, a small SIMT ISA that the tutorial implements one chapter at a time.
//
// Chapter 00 keeps this file deliberately empty of policy: one processor, an
// empty feature set, and no real instructions.  Chapters 01-02 add the GM32
// instruction formats and the ISA; chapters 11-18 add the SIMT features
// (lanes per warp, warps per block) and the machine model.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Processors
//===----------------------------------------------------------------------===//
//
// One processor is enough while the ISA has no optional extensions.  The name
// is what `llc -mcpu=` accepts; the default subtarget uses "generic".

class GPUMinProc<string Name> : Processor<Name, NoItineraries, []>;

def : GPUMinProc<"generic">;

//===----------------------------------------------------------------------===//
// Register File
//===----------------------------------------------------------------------===//

include "GPUMinRegisterInfo.td"

//===----------------------------------------------------------------------===//
// Instructions
//===----------------------------------------------------------------------===//

include "GPUMinInstrInfo.td"

include "GPUMinCallingConv.td"

// Pointer operands of the target-independent pseudo instructions
// (PREALLOCATED_ARG, LOAD_STACK_GUARD, ...) are memoery operands; remap them
// onto the GPUMin GPR class so the generic expansions can use registers.
defm : RemapAllTargetPseudoPointerOperands<GPR>;

def GPUMinInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// Assembly parser / writer
//===----------------------------------------------------------------------===//

// The printer is hand written in MCTargetDesc/GPUMinInstPrinter.cpp on top of
// the generated per-instruction printing code.
def GPUMinAsmWriter : AsmWriter {
  let AsmWriterClassName = "InstPrinter";
  int PassSubtarget = 1;
}

// The parser is hand written; TableGen generates the matcher tables from the
// instruction AsmStrings and operand match classes.
def GPUMinAsmParser : AsmParser {
  let ShouldEmitMatchRegisterName = true;
}

//===----------------------------------------------------------------------===//
// Target Declaration
//===----------------------------------------------------------------------===//

def GPUMin : Target {
  let InstructionSet = GPUMinInstrInfo;
  let AssemblyParsers = [GPUMinAsmParser];
  let AssemblyWriters = [GPUMinAsmWriter];
}
