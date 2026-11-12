//===-- RVTrim.h - Top-level interface for RISC-V ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in the LLVM
// RISC-V back-end.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVTrim_RVTrim_H
#define LLVM_LIB_TARGET_RVTrim_RVTrim_H

#include "MCTargetDesc/RVTrimBaseInfo.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
class FunctionPass;
class InstructionSelector;
class ModulePass;
class PassRegistry;
class RVTrimRegisterBankInfo;
class RVTrimSubtarget;
class RVTrimTargetMachine;

class RVTrimCodeGenPreparePass
    : public OptionalPassInfoMixin<RVTrimCodeGenPreparePass> {
private:
  const RVTrimTargetMachine *TM;

public:
  RVTrimCodeGenPreparePass(const RVTrimTargetMachine *TM) : TM(TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};
FunctionPass *createRVTrimCodeGenPrepareLegacyPass();
void initializeRVTrimCodeGenPrepareLegacyPass(PassRegistry &);

class RVTrimDeadRegisterDefinitionsPass
    : public OptionalPassInfoMixin<RVTrimDeadRegisterDefinitionsPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};
FunctionPass *createRVTrimDeadRegisterDefinitionsLegacyPass();
void initializeRVTrimDeadRegisterDefinitionsLegacyPass(PassRegistry &);

class RVTrimIndirectBranchTrackingPass
    : public RequiredPassInfoMixin<RVTrimIndirectBranchTrackingPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};
FunctionPass *createRVTrimIndirectBranchTrackingLegacyPass();
void initializeRVTrimIndirectBranchTrackingLegacyPass(PassRegistry &);

class RVTrimLandingPadSetupPass
    : public RequiredPassInfoMixin<RVTrimLandingPadSetupPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};
FunctionPass *createRVTrimLandingPadSetupLegacyPass();
void initializeRVTrimLandingPadSetupLegacyPass(PassRegistry &);

FunctionPass *createRVTrimISelDagLegacyPass(RVTrimTargetMachine &TM,
                                           CodeGenOptLevel OptLevel);

class RVTrimISelDAGToDAGPass : public SelectionDAGISelPass {
public:
  RVTrimISelDAGToDAGPass(RVTrimTargetMachine &TM, CodeGenOptLevel OptLevel);
};

class RVTrimLateBranchOptPass
    : public OptionalPassInfoMixin<RVTrimLateBranchOptPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};
FunctionPass *createRVTrimLateBranchOptLegacyPass();
void initializeRVTrimLateBranchOptLegacyPass(PassRegistry &);

class RVTrimMakeCompressiblePass
    : public OptionalPassInfoMixin<RVTrimMakeCompressiblePass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};
FunctionPass *createRVTrimMakeCompressibleLegacyPass();
void initializeRVTrimMakeCompressibleLegacyPass(PassRegistry &);

class RVTrimGatherScatterLoweringPass
    : public OptionalPassInfoMixin<RVTrimGatherScatterLoweringPass> {
private:
  const RVTrimTargetMachine *TM;

public:
  RVTrimGatherScatterLoweringPass(const RVTrimTargetMachine *TM) : TM(TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

FunctionPass *createRVTrimGatherScatterLoweringLegacyPass();
void initializeRVTrimGatherScatterLoweringLegacyPass(PassRegistry &);

class RVTrimVectorPeepholePass
    : public OptionalPassInfoMixin<RVTrimVectorPeepholePass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
  MachineFunctionProperties getRequiredProperties() const {
    return MachineFunctionProperties().setIsSSA();
  }
};

FunctionPass *createRVTrimVectorPeepholeLegacyPass();
void initializeRVTrimVectorPeepholeLegacyPass(PassRegistry &);

class RVTrimOptWInstrsPass : public OptionalPassInfoMixin<RVTrimOptWInstrsPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimOptWInstrsLegacyPass();
void initializeRVTrimOptWInstrsLegacyPass(PassRegistry &);

class RVTrimFoldMemOffsetPass
    : public OptionalPassInfoMixin<RVTrimFoldMemOffsetPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimFoldMemOffsetLegacyPass();
void initializeRVTrimFoldMemOffsetLegacyPass(PassRegistry &);

class RVTrimMergeBaseOffsetPass
    : public OptionalPassInfoMixin<RVTrimMergeBaseOffsetPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimMergeBaseOffsetLegacyPass();
void initializeRVTrimMergeBaseOffsetLegacyPass(PassRegistry &);

class RVTrimExpandPseudoPreRAPass
    : public RequiredPassInfoMixin<RVTrimExpandPseudoPreRAPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
  MachineFunctionProperties getRequiredProperties() const {
    return MachineFunctionProperties().setIsSSA();
  }
};

FunctionPass *createRVTrimExpandPseudoPreRALegacyPass();
void initializeRVTrimExpandPseudoPreRALegacyPass(PassRegistry &);

class RVTrimExpandPseudoPostRAPass
    : public RequiredPassInfoMixin<RVTrimExpandPseudoPostRAPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimExpandPseudoPostRALegacyPass();
void initializeRVTrimExpandPseudoPostRALegacyPass(PassRegistry &);

class RVTrimExpandPseudoPreEmitPass
    : public RequiredPassInfoMixin<RVTrimExpandPseudoPreEmitPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimExpandPseudoPreEmitLegacyPass();
void initializeRVTrimExpandPseudoPreEmitLegacyPass(PassRegistry &);

class RVTrimExpandPseudoAtomicsPass
    : public RequiredPassInfoMixin<RVTrimExpandPseudoAtomicsPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimExpandPseudoAtomicsLegacyPass();
void initializeRVTrimExpandPseudoAtomicsLegacyPass(PassRegistry &);

class RVTrimInsertVSETVLIPass
    : public RequiredPassInfoMixin<RVTrimInsertVSETVLIPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimInsertVSETVLILegacyPass();
void initializeRVTrimInsertVSETVLILegacyPass(PassRegistry &);
extern char &RVTrimInsertVSETVLILegacyID;

class RVTrimInsertReadWriteCSRPass
    : public RequiredPassInfoMixin<RVTrimInsertReadWriteCSRPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimInsertReadWriteCSRLegacyPass();
void initializeRVTrimInsertReadWriteCSRLegacyPass(PassRegistry &);

class RVTrimInsertWriteVXRMPass
    : public RequiredPassInfoMixin<RVTrimInsertWriteVXRMPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimInsertWriteVXRMLegacyPass();
void initializeRVTrimInsertWriteVXRMLegacyPass(PassRegistry &);

class RVTrimRedundantCopyEliminationPass
    : public OptionalPassInfoMixin<RVTrimRedundantCopyEliminationPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);

  MachineFunctionProperties getRequiredProperties() const {
    return MachineFunctionProperties().setNoVRegs();
  }
};

FunctionPass *createRVTrimRedundantCopyEliminationLegacyPass();
void initializeRVTrimRedundantCopyEliminationLegacyPass(PassRegistry &);

class RVTrimMoveMergePass : public OptionalPassInfoMixin<RVTrimMoveMergePass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimMoveMergeLegacyPass();
void initializeRVTrimMoveMergeLegacyPass(PassRegistry &);

class RVTrimPushPopOptPass : public OptionalPassInfoMixin<RVTrimPushPopOptPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimPushPopOptLegacyPass();
void initializeRVTrimPushPopOptLegacyPass(PassRegistry &);

class RVTrimLoadStoreOptPass
    : public OptionalPassInfoMixin<RVTrimLoadStoreOptPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimLoadStoreOptLegacyPass();
void initializeRVTrimLoadStoreOptLegacyPass(PassRegistry &);

class RVTrimPreAllocZilsdOptPass
    : public OptionalPassInfoMixin<RVTrimPreAllocZilsdOptPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimPreAllocZilsdOptLegacyPass();
void initializeRVTrimPreAllocZilsdOptLegacyPass(PassRegistry &);

class RVTrimZacasABIFixPass
    : public RequiredPassInfoMixin<RVTrimZacasABIFixPass> {
private:
  const RVTrimTargetMachine *TM;

public:
  RVTrimZacasABIFixPass(const RVTrimTargetMachine *TM) : TM(TM) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};
FunctionPass *createRVTrimZacasABIFixLegacyPass();
void initializeRVTrimZacasABIFixLegacyPass(PassRegistry &);

InstructionSelector *
createRVTrimInstructionSelector(const RVTrimTargetMachine &,
                               const RVTrimSubtarget &,
                               const RVTrimRegisterBankInfo &);
void initializeRVTrimDAGToDAGISelLegacyPass(PassRegistry &);

FunctionPass *createRVTrimPostLegalizerCombiner();
void initializeRVTrimPostLegalizerCombinerPass(PassRegistry &);

FunctionPass *createRVTrimO0PreLegalizerCombiner();
void initializeRVTrimO0PreLegalizerCombinerPass(PassRegistry &);

FunctionPass *createRVTrimPreLegalizerCombiner();
void initializeRVTrimPreLegalizerCombinerPass(PassRegistry &);

class RVTrimPromoteConstantPass
    : public OptionalPassInfoMixin<RVTrimPromoteConstantPass> {
private:
  const RVTrimTargetMachine *TM;

public:
  RVTrimPromoteConstantPass(const RVTrimTargetMachine *TM) : TM(TM) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

ModulePass *createRVTrimPromoteConstantLegacyPass();
void initializeRVTrimPromoteConstantLegacyPass(PassRegistry &);

class RVTrimVLOptimizerPass
    : public OptionalPassInfoMixin<RVTrimVLOptimizerPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimVLOptimizerLegacyPass();
void initializeRVTrimVLOptimizerLegacyPass(PassRegistry &);

class RVTrimVMV0EliminationPass
    : public RequiredPassInfoMixin<RVTrimVMV0EliminationPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);

  MachineFunctionProperties getRequiredProperties() const {
    return MachineFunctionProperties().setIsSSA();
  }
};

FunctionPass *createRVTrimVMV0EliminationLegacyPass();
void initializeRVTrimVMV0EliminationLegacyPass(PassRegistry &);

class RVTrimQCRelaxMarkingPass
    : public OptionalPassInfoMixin<RVTrimQCRelaxMarkingPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createRVTrimQCRelaxMarkingLegacyPass();
void initializeRVTrimQCRelaxMarkingLegacyPass(PassRegistry &);

void initializeRVTrimAsmPrinterPass(PassRegistry &);
} // namespace llvm

#endif
