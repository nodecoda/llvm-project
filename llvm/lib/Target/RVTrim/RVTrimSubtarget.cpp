//===-- RVTrimSubtarget.cpp - RISC-V Subtarget Information -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the RISC-V specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "RVTrimSubtarget.h"
#include "GISel/RVTrimCallLowering.h"
#include "GISel/RVTrimInlineAsmLowering.h"
#include "GISel/RVTrimLegalizerInfo.h"
#include "RVTrim.h"
#include "RVTrimFrameLowering.h"
#include "RVTrimSelectionDAGInfo.h"
#include "RVTrimTargetMachine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineTraceMetrics.h"
#include "llvm/MC/MCSchedule.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Option/LibraryOptions.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "rvtrim-macro-fusion"

#define GET_RVTrim_MACRO_FUSION_PRED_IMPL
#include "RVTrimGenMacroFusion.inc"

#undef DEBUG_TYPE
#define DEBUG_TYPE "rvtrim-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "RVTrimGenSubtargetInfo.inc"

#define OPTIONS_STRUCT_DEFS
#include "RVTrimOptions.inc"

namespace llvm::RVTrimTuneInfoTable {

#define GET_RVTrimTuneInfoTable_IMPL
#include "RVTrimGenSearchableTables.inc"
} // namespace llvm::RVTrimTuneInfoTable

void RVTrimSubtarget::anchor() {}

RVTrimSubtarget &
RVTrimSubtarget::initializeSubtargetDependencies(const Triple &TT, StringRef CPU,
                                                StringRef TuneCPU, StringRef FS,
                                                StringRef ABIName) {
  // Determine default and user-specified characteristics
  bool Is64Bit = TT.isArch64Bit();
  if (CPU.empty() || CPU == "generic")
    CPU = Is64Bit ? "generic-rv64" : "generic-rv32";

  if (TuneCPU.empty())
    TuneCPU = CPU;
  if (TuneCPU == "generic")
    TuneCPU = Is64Bit ? "generic-rv64" : "generic-rv32";

  TuneInfo = RVTrimTuneInfoTable::getRVTrimTuneInfo(TuneCPU);
  // If there is no TuneInfo for this CPU, we fail back to generic.
  if (!TuneInfo)
    TuneInfo = RVTrimTuneInfoTable::getRVTrimTuneInfo("generic");
  assert(TuneInfo && "TuneInfo shouldn't be nullptr!");

  ParseSubtargetFeatures(CPU, TuneCPU, FS);

  RVTrim::updateCZceFeatureImplications(*this);

  // Re-sync the flags.
  HasStdExtZcd = hasFeature(RVTrim::FeatureStdExtZcd);
  HasStdExtZcf = hasFeature(RVTrim::FeatureStdExtZcf);
  HasStdExtC = hasFeature(RVTrim::FeatureStdExtC);
  HasStdExtZce = hasFeature(RVTrim::FeatureStdExtZce);

  // Can't be fatal: per-function subtargets mean this one may just be the
  // module-level default with no matching function, e.g. -target-abi ilp32f
  // with no global -mattr=+f but all functions have their own "+f" attribute.
  if (auto ABIOrErr = RVTrimABI::computeTargetABI(*this, ABIName)) {
    TargetABI = *ABIOrErr;
  } else {
    errs() << "note: " << toString(ABIOrErr.takeError())
           << " (ignoring target-abi)\n";
    TargetABI = cantFail(RVTrimABI::computeTargetABI(*this, ""));
  }
  RVTrimFeatures::validate(TT, getFeatureBits());
  RVTrimFeatures::validateProfile(*this);
  return *this;
}

RVTrimSubtarget::RVTrimSubtarget(const Triple &TT, StringRef CPU,
                               StringRef TuneCPU, StringRef FS,
                               StringRef ABIName, unsigned RVVVectorBitsMin,
                               unsigned RVVVectorBitsMax,
                               const TargetMachine &TM)
    : RVTrimGenSubtargetInfo(TT, CPU, TuneCPU, FS),
      CLOpts(static_cast<const RVTrimTargetMachine &>(TM).getCLOpts()),
      IsLittleEndian(TT.isLittleEndian()), RVVVectorBitsMin(RVVVectorBitsMin),
      RVVVectorBitsMax(RVVVectorBitsMax),
      FrameLowering(
          initializeSubtargetDependencies(TT, CPU, TuneCPU, FS, ABIName)),
      InstrInfo(*this), TLInfo(TM, *this) {
  TSInfo = std::make_unique<RVTrimSelectionDAGInfo>();
}

RVTrimSubtarget::~RVTrimSubtarget() = default;

const SelectionDAGTargetInfo *RVTrimSubtarget::getSelectionDAGInfo() const {
  return TSInfo.get();
}

const InlineAsmLowering *RVTrimSubtarget::getInlineAsmLowering() const {
  if (!InlineAsmLoweringInfo)
    InlineAsmLoweringInfo.reset(
        new RVTrimInlineAsmLowering(getTargetLowering()));
  return InlineAsmLoweringInfo.get();
}

const CallLowering *RVTrimSubtarget::getCallLowering() const {
  if (!CallLoweringInfo)
    CallLoweringInfo.reset(new RVTrimCallLowering(*getTargetLowering()));
  return CallLoweringInfo.get();
}

InstructionSelector *RVTrimSubtarget::getInstructionSelector() const {
  if (!InstSelector) {
    InstSelector.reset(createRVTrimInstructionSelector(
        *static_cast<const RVTrimTargetMachine *>(&TLInfo.getTargetMachine()),
        *this, *getRegBankInfo()));
  }
  return InstSelector.get();
}

const LegalizerInfo *RVTrimSubtarget::getLegalizerInfo() const {
  if (!Legalizer)
    Legalizer.reset(new RVTrimLegalizerInfo(*this));
  return Legalizer.get();
}

const RVTrimRegisterBankInfo *RVTrimSubtarget::getRegBankInfo() const {
  if (!RegBankInfo)
    RegBankInfo.reset(new RVTrimRegisterBankInfo(getHwMode()));
  return RegBankInfo.get();
}

bool RVTrimSubtarget::useConstantPoolForLargeInts() const {
  return CLOpts.constant_pool_for_large_ints;
}

// Returns true if VT is a P extension packed SIMD type.
bool RVTrimSubtarget::isPExtPackedType(MVT VT) const {
  if (!HasStdExtP)
    return false;

  // RV32 supports 32-bit and 64-bit vectors. RV64 only support 64-bit vectors.
  if (!is64Bit() && (VT == MVT::v4i8 || VT == MVT::v2i16))
    return true;

  return VT == MVT::v8i8 || VT == MVT::v4i16 || VT == MVT::v2i32;
}

// Returns true if VT is a P extension packed double-wide SIMD type.
bool RVTrimSubtarget::isPExtPackedDoubleType(MVT VT) const {
  if (!HasStdExtP || is64Bit())
    return false;

  return VT == MVT::v8i8 || VT == MVT::v4i16 || VT == MVT::v2i32;
}

unsigned RVTrimSubtarget::getMaxBuildIntsCost() const {
  // Loading integer from constant pool needs two instructions (the reason why
  // the minimum cost is 2): an address calculation instruction and a load
  // instruction. Usually, address calculation and instructions used for
  // building integers (addi, slli, etc.) can be done in one cycle, so here we
  // set the default cost to (LoadLatency + 1) if no threshold is provided.
  return CLOpts.max_build_ints_cost == 0
             ? getLoadLatency() + 1
             : std::max<unsigned>(2, CLOpts.max_build_ints_cost);
}

unsigned RVTrimSubtarget::getMispredictionPenalty() const {
  return CLOpts.sched_mispredict_penalty.value_or(
      getSchedModel().MispredictPenalty);
}

unsigned RVTrimSubtarget::getLoadLatency() const {
  return CLOpts.sched_load_latency.value_or(getSchedModel().LoadLatency);
}

unsigned RVTrimSubtarget::getMaxRVVVectorSizeInBits() const {
  assert(hasVInstructions() &&
         "Tried to get vector length without Zve or V extension support!");

  // ZvlLen specifies the minimum required vlen. The upper bound provided by
  // rvtrim-v-vector-bits-max should be no less than it.
  if (RVVVectorBitsMax != 0 && RVVVectorBitsMax < ZvlLen)
    report_fatal_error("rvtrim-v-vector-bits-max specified is lower "
                       "than the Zvl*b limitation");

  return RVVVectorBitsMax;
}

unsigned RVTrimSubtarget::getMinRVVVectorSizeInBits() const {
  assert(hasVInstructions() &&
         "Tried to get vector length without Zve or V extension support!");

  if (RVVVectorBitsMin == -1U)
    return ZvlLen;

  // ZvlLen specifies the minimum required vlen. The lower bound provided by
  // rvtrim-v-vector-bits-min should be no less than it.
  if (RVVVectorBitsMin != 0 && RVVVectorBitsMin < ZvlLen)
    report_fatal_error("rvtrim-v-vector-bits-min specified is lower "
                       "than the Zvl*b limitation");

  return RVVVectorBitsMin;
}

bool RVTrimSubtarget::useRVVForFixedLengthVectors() const {
  return hasVInstructions() &&
         getMinRVVVectorSizeInBits() >= RISCV::RVVBitsPerBlock;
}

bool RVTrimSubtarget::enableSubRegLiveness() const { return true; }

bool RVTrimSubtarget::enableMachinePipeliner() const {
  return getSchedModel().hasInstrSchedModel();
}

void RVTrimSubtarget::mirFileLoaded(MachineFunction &MF) const {
  // We usually compute max call frame size after ISel. Do the computation now
  // if the .mir file didn't specify it. Note that this will probably give you
  // bogus values after PEI has eliminated the callframe setup/destroy pseudo
  // instructions, specify explicitly if you need it to be correct.
  MachineFrameInfo &MFI = MF.getFrameInfo();
  if (!MFI.isMaxCallFrameSizeComputed())
    MFI.computeMaxCallFrameSize(MF);
}

  /// Enable use of alias analysis during code generation (during MI
  /// scheduling, DAGCombine, etc.).
bool RVTrimSubtarget::useAA() const { return CLOpts.use_aa; }

unsigned RVTrimSubtarget::getMinimumJumpTableEntries() const {
  return CLOpts.min_jump_table_entries.value_or(
      TuneInfo->MinimumJumpTableEntries);
}

void RVTrimSubtarget::overrideSchedPolicy(MachineSchedPolicy &Policy,
                                         const SchedRegion &Region) const {
  // Do bidirectional scheduling since it provides a more balanced scheduling
  // leading to better performance. This will increase compile time.
  Policy.OnlyTopDown = false;
  Policy.OnlyBottomUp = false;

  // Disabling the latency heuristic can reduce the number of spills/reloads but
  // will cause some regressions on some cores.
  Policy.DisableLatencyHeuristic = DisableLatencySchedHeuristic;

  // Spilling is generally expensive on all RISC-V cores, so always enable
  // register-pressure tracking. This will increase compile time.
  Policy.ShouldTrackPressure = true;
}

void RVTrimSubtarget::overridePostRASchedPolicy(
    MachineSchedPolicy &Policy, const SchedRegion &Region) const {
  MISched::Direction PostRASchedDirection = getPostRASchedDirection();
  if (PostRASchedDirection == MISched::TopDown) {
    Policy.OnlyTopDown = true;
    Policy.OnlyBottomUp = false;
  } else if (PostRASchedDirection == MISched::BottomUp) {
    Policy.OnlyTopDown = false;
    Policy.OnlyBottomUp = true;
  } else if (PostRASchedDirection == MISched::Bidirectional) {
    Policy.OnlyTopDown = false;
    Policy.OnlyBottomUp = false;
  }
}

bool RVTrimSubtarget::useMIPSLoadStorePairs() const {
  return CLOpts.mips_load_store_pairs && HasVendorXMIPSLSP;
}

bool RVTrimSubtarget::useMIPSCCMovInsn() const {
  return CLOpts.mips_ccmov && HasVendorXMIPSCMov;
}
