//===--- RVTrimMachineScheduler.h - Custom RISC-V MI scheduler ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Custom RISC-V MI scheduler.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVTrim_RVTrimMACHINESCHEDULER_H
#define LLVM_LIB_TARGET_RVTrim_RVTrimMACHINESCHEDULER_H

#include "RVTrimSubtarget.h"
#include "RVTrimVSETVLIInfoAnalysis.h"
#include "llvm/CodeGen/MachineScheduler.h"

namespace llvm {

/// A GenericScheduler implementation for RVTrim pre RA scheduling.
class RVTrimPreRAMachineSchedStrategy : public GenericScheduler {
  const RVTrimSubtarget *ST;
  RVTrim::RVTrimVSETVLIInfoAnalysis VIA;
  RVTrim::VSETVLIInfo TopInfo;
  RVTrim::VSETVLIInfo BottomInfo;

  RVTrim::VSETVLIInfo getVSETVLIInfo(const MachineInstr *MI) const;
  bool tryVSETVLIInfo(const RVTrim::VSETVLIInfo &TryInfo,
                      const RVTrim::VSETVLIInfo &CandInfo,
                      SchedCandidate &TryCand, SchedCandidate &Cand,
                      CandReason Reason) const;

public:
  RVTrimPreRAMachineSchedStrategy(const MachineSchedContext *C)
      : GenericScheduler(C), ST(&C->MF->getSubtarget<RVTrimSubtarget>()),
        VIA(ST, C->LIS) {}

protected:
  bool tryCandidate(SchedCandidate &Cand, SchedCandidate &TryCand,
                    SchedBoundary *Zone) const override;
  void enterMBB(MachineBasicBlock *MBB) override;
  void leaveMBB() override;
  void schedNode(SUnit *SU, bool IsTopNode) override;
};

} // end namespace llvm

#endif
