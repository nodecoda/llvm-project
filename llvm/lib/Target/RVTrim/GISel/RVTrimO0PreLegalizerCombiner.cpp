//=== RVTrimO0PreLegalizerCombiner.cpp -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass does combining of machine instructions at the generic MI level,
// before the legalizer.
//
//===----------------------------------------------------------------------===//

#include "RVTrimSubtarget.h"
#include "llvm/CodeGen/GlobalISel/Combiner.h"
#include "llvm/CodeGen/GlobalISel/CombinerHelper.h"
#include "llvm/CodeGen/GlobalISel/CombinerInfo.h"
#include "llvm/CodeGen/GlobalISel/GIMatchTableExecutorImpl.h"
#include "llvm/CodeGen/GlobalISel/GISelValueTracking.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

#define GET_GICOMBINER_DEPS
#include "RVTrimGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_DEPS

#define DEBUG_TYPE "rvtrim-O0-prelegalizer-combiner"

using namespace llvm;

namespace {
#define GET_GICOMBINER_TYPES
#include "RVTrimGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_TYPES

class RVTrimO0PreLegalizerCombinerImpl : public Combiner {
protected:
  const CombinerHelper Helper;
  const RVTrimO0PreLegalizerCombinerImplRuleConfig &RuleConfig;
  const RVTrimSubtarget &STI;

public:
  RVTrimO0PreLegalizerCombinerImpl(
      MachineFunction &MF, CombinerInfo &CInfo, GISelValueTracking &VT,
      GISelCSEInfo *CSEInfo,
      const RVTrimO0PreLegalizerCombinerImplRuleConfig &RuleConfig,
      const RVTrimSubtarget &STI);

  static const char *getName() { return "RVTrimO0PreLegalizerCombiner"; }

  bool tryCombineAll(MachineInstr &I) const override;

private:
#define GET_GICOMBINER_CLASS_MEMBERS
#include "RVTrimGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CLASS_MEMBERS
};

#define GET_GICOMBINER_IMPL
#include "RVTrimGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_IMPL

RVTrimO0PreLegalizerCombinerImpl::RVTrimO0PreLegalizerCombinerImpl(
    MachineFunction &MF, CombinerInfo &CInfo, GISelValueTracking &VT,
    GISelCSEInfo *CSEInfo,
    const RVTrimO0PreLegalizerCombinerImplRuleConfig &RuleConfig,
    const RVTrimSubtarget &STI)
    : Combiner(MF, CInfo, &VT, CSEInfo),
      Helper(Observer, B, /*IsPreLegalize*/ true, &VT), RuleConfig(RuleConfig),
      STI(STI),
#define GET_GICOMBINER_CONSTRUCTOR_INITS
#include "RVTrimGenO0PreLegalizeGICombiner.inc"
#undef GET_GICOMBINER_CONSTRUCTOR_INITS
{
}

// Pass boilerplate
// ================

class RVTrimO0PreLegalizerCombiner : public MachineFunctionPass {
public:
  static char ID;

  RVTrimO0PreLegalizerCombiner();

  StringRef getPassName() const override {
    return "RVTrimO0PreLegalizerCombiner";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  RVTrimO0PreLegalizerCombinerImplRuleConfig RuleConfig;
};
} // end anonymous namespace

void RVTrimO0PreLegalizerCombiner::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  getSelectionDAGFallbackAnalysisUsage(AU);
  AU.addRequired<GISelValueTrackingAnalysisLegacy>();
  AU.addPreserved<GISelValueTrackingAnalysisLegacy>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

RVTrimO0PreLegalizerCombiner::RVTrimO0PreLegalizerCombiner()
    : MachineFunctionPass(ID) {
  if (!RuleConfig.parseCommandLineOption())
    report_fatal_error("Invalid rule identifier");
}

bool RVTrimO0PreLegalizerCombiner::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getProperties().hasFailedISel())
    return false;

  const Function &F = MF.getFunction();
  GISelValueTracking *VT =
      &getAnalysis<GISelValueTrackingAnalysisLegacy>().get(MF);

  const RVTrimSubtarget &ST = MF.getSubtarget<RVTrimSubtarget>();

  CombinerInfo CInfo(/*AllowIllegalOps*/ true, /*ShouldLegalizeIllegal*/ false,
                     /*LegalizerInfo*/ nullptr, /*EnableOpt*/ false,
                     F.hasOptSize(), F.hasMinSize());
  // Disable fixed-point iteration in the Combiner. This improves compile-time
  // at the cost of possibly missing optimizations. See PR#94291 for details.
  CInfo.MaxIterations = 1;

  RVTrimO0PreLegalizerCombinerImpl Impl(MF, CInfo, *VT,
                                       /*CSEInfo*/ nullptr, RuleConfig, ST);
  return Impl.combineMachineInstrs();
}

char RVTrimO0PreLegalizerCombiner::ID = 0;
INITIALIZE_PASS_BEGIN(RVTrimO0PreLegalizerCombiner, DEBUG_TYPE,
                      "Combine RISC-V machine instrs before legalization",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(GISelValueTrackingAnalysisLegacy)
INITIALIZE_PASS_DEPENDENCY(GISelCSEAnalysisWrapperPass)
INITIALIZE_PASS_END(RVTrimO0PreLegalizerCombiner, DEBUG_TYPE,
                    "Combine RISC-V machine instrs before legalization", false,
                    false)

FunctionPass *llvm::createRVTrimO0PreLegalizerCombiner() {
  return new RVTrimO0PreLegalizerCombiner();
}
