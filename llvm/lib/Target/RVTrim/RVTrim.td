//===-- RVTrim.td - Describe the RISC-V Target Machine ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// RISC-V subtarget features and instruction predicates.
//===----------------------------------------------------------------------===//

include "RVTrimFeatures.td"

//===----------------------------------------------------------------------===//
// RISC-V profiles supported.
//===----------------------------------------------------------------------===//

include "RVTrimProfiles.td"

//===----------------------------------------------------------------------===//
// Named operands for CSR instructions.
//===----------------------------------------------------------------------===//

include "RVTrimSystemOperands.td"

//===----------------------------------------------------------------------===//
// Registers, calling conventions, instruction descriptions.
//===----------------------------------------------------------------------===//

include "RVTrimRegisterInfo.td"
include "RVTrimSchedule.td"
include "RVTrimCallingConv.td"
include "RVTrimInstrInfo.td"
include "GISel/RVTrimRegisterBanks.td"

//===----------------------------------------------------------------------===//
// Instruction predicates
//===----------------------------------------------------------------------===//

include "RVTrimInstrPredicates.td"

//===----------------------------------------------------------------------===//
// RISC-V macro fusions.
//===----------------------------------------------------------------------===//

include "RVTrimMacroFusion.td"

//===----------------------------------------------------------------------===//
// RISC-V Scheduling Models
//===----------------------------------------------------------------------===//
include "RVTrimSchedAndes45.td"
include "RVTrimSchedGenericOOO.td"
include "RVTrimSchedMIPSP8700.td"
include "RVTrimSchedRocket.td"
include "RVTrimSchedSiFive7.td"
include "RVTrimSchedSiFiveP400.td"
include "RVTrimSchedSiFiveP500.td"
include "RVTrimSchedSiFiveP600.td"
include "RVTrimSchedSiFiveP800.td"
include "RVTrimSchedSpacemitX60.td"
include "RVTrimSchedSpacemitX100.td"
include "RVTrimSchedSyntacoreSCR1.td"
include "RVTrimSchedSyntacoreSCR345.td"
include "RVTrimSchedSyntacoreSCR7.td"
include "RVTrimSchedTTAscalonX.td"
include "RVTrimSchedXiangShanKunMingHu.td"
include "RVTrimSchedXiangShanNanHu.td"

//===----------------------------------------------------------------------===//
// RISC-V processors supported.
//===----------------------------------------------------------------------===//

include "RVTrimProcessors.td"

//===----------------------------------------------------------------------===//
// Pfm Counters
//===----------------------------------------------------------------------===//

include "RVTrimPfmCounters.td"

//===----------------------------------------------------------------------===//
// Define the RISC-V target.
//===----------------------------------------------------------------------===//

def RVTrimInstrInfo : InstrInfo {
  let guessInstructionProperties = false;
}

def RVTrimAsmParser : AsmParser {
  let ShouldEmitMatchRegisterAltName = true;
  let AllowDuplicateRegisterNames = true;
  let ReportMultipleNearMisses = true;
  let PrioritizeFeatureInMultiMismatchFallback = true;
  let EmitTokenDiagnosticTypes = true;
}

def RVTrimAsmWriter : AsmWriter {
  int PassSubtarget = 1;
}

defm : RemapAllTargetPseudoPointerOperands<GPR>;

def RVTrim : Target {
  let InstructionSet = RVTrimInstrInfo;
  let AssemblyParsers = [RVTrimAsmParser];
  let AssemblyWriters = [RVTrimAsmWriter];
  let AllowRegisterRenaming = true;
}
