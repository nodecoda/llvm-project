//===-------------- RVTrimVLOptimizer.cpp - VL Optimizer -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This pass reduces the VL where possible at the MI level, before VSETVLI
// instructions are inserted.
//
// The purpose of this optimization is to make the VL argument, for instructions
// that have a VL argument, as small as possible.
//
// This is split into a sparse dataflow analysis where we determine what VL is
// demanded by each instruction first, and then afterwards try to reduce the VL
// of each instruction if it demands less than its VL operand.
//
// The analysis is explained in more detail in the 2025 EuroLLVM Developers'
// Meeting talk "Accidental Dataflow Analysis: Extending the RISC-V VL
// Optimizer", which is available on YouTube at
// https://www.youtube.com/watch?v=Mfb5fRSdJAc
//
// The slides for the talk are available at
// https://llvm.org/devmtg/2025-04/slides/technical_talk/lau_accidental_dataflow.pdf
//
//===---------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimSubtarget.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

#define DEBUG_TYPE "rvtrim-vl-optimizer"
#define PASS_NAME "RISC-V VL Optimizer"

namespace {

/// Wrapper around MachineOperand that defaults to immediate 0.
struct DemandedVL {
  MachineOperand VL;
  DemandedVL() : VL(MachineOperand::CreateImm(0)) {}
  DemandedVL(MachineOperand VL) : VL(VL) {}
  static DemandedVL vlmax() {
    return DemandedVL(MachineOperand::CreateImm(RVTrim::VLMaxSentinel));
  }
  bool operator!=(const DemandedVL &Other) const {
    return !VL.isIdenticalTo(Other.VL);
  }

  DemandedVL max(const MachineRegisterInfo &MRI, const DemandedVL &X) const {
    if (RVTrim::isVLKnownLE(MRI, VL, X.VL))
      return X;
    if (RVTrim::isVLKnownLE(MRI, X.VL, VL))
      return *this;
    return DemandedVL::vlmax();
  }
};

class RVTrimVLOptimizerImpl {
  MachineRegisterInfo *MRI;
  function_ref<const MachineDominatorTree &()> GetMDT;
  const MachineDominatorTree *MDT = nullptr;
  const TargetInstrInfo *TII;

public:
  RVTrimVLOptimizerImpl(function_ref<const MachineDominatorTree &()> GetMDT)
      : GetMDT(GetMDT) {}

  bool run(MachineFunction &MF);

private:
  DemandedVL getMinimumVLForUser(const MachineInstr &UserMI,
                                 unsigned OpIdx) const;
  /// Returns true if the users of \p MI have compatible EEWs and SEWs.
  bool checkUsers(const MachineInstr &MI) const;
  bool tryReduceVL(MachineInstr &MI, MachineOperand VL);
  bool isSupportedInstr(const MachineInstr &MI) const;
  bool isCandidate(const MachineInstr &MI) const;
  void transfer(const MachineInstr &MI);

  /// For a given instruction, records what elements of it are demanded by
  /// downstream users.
  MapVector<const MachineInstr *, DemandedVL> DemandedVLs;
  SetVector<const MachineInstr *> Worklist;

  /// \returns all vector virtual registers that \p MI uses.
  auto virtual_vec_uses(const MachineInstr &MI) const {
    return make_filter_range(MI.uses(), [this](const MachineOperand &MO) {
      return MO.isReg() && MO.getReg().isVirtual() &&
             RVTrimRegisterInfo::isRVVRegClass(MRI->getRegClass(MO.getReg()));
    });
  }

  /// \returns all vector virtual registers that \p MI defines.
  auto virtual_vec_defs(const MachineInstr &MI) const {
    return make_filter_range(MI.all_defs(), [this](const MachineOperand &MO) {
      return MO.getReg().isVirtual() &&
             RVTrimRegisterInfo::isRVVRegClass(MRI->getRegClass(MO.getReg()));
    });
  }
};

class RVTrimVLOptimizerLegacy : public MachineFunctionPass {
public:
  static char ID;

  RVTrimVLOptimizerLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineDominatorTreeWrapperPass>();
    AU.addPreserved<MachineRegisterClassInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return PASS_NAME; }
};

/// Represents the EMUL and EEW of a MachineOperand.
struct OperandInfo {
  // Represent as 1,2,4,8, ... and fractional indicator. This is because
  // EMUL can take on values that don't map to RISCVVType::VLMUL values exactly.
  // For example, a mask operand can have an EMUL less than MF8.
  // If nullopt, then EMUL isn't used (i.e. only a single scalar is read).
  std::optional<std::pair<unsigned, bool>> EMUL;

  unsigned Log2EEW;

  OperandInfo(RISCVVType::VLMUL EMUL, unsigned Log2EEW)
      : EMUL(RISCVVType::decodeVLMUL(EMUL)), Log2EEW(Log2EEW) {}

  OperandInfo(std::pair<unsigned, bool> EMUL, unsigned Log2EEW)
      : EMUL(EMUL), Log2EEW(Log2EEW) {}

  OperandInfo(unsigned Log2EEW) : Log2EEW(Log2EEW) {}

  OperandInfo() = delete;

  /// Return true if the EMUL and EEW produced by \p Def is compatible with the
  /// EMUL and EEW used by \p User.
  static bool areCompatible(const OperandInfo &Def, const OperandInfo &User) {
    if (Def.Log2EEW != User.Log2EEW)
      return false;
    if (User.EMUL && Def.EMUL != User.EMUL)
      return false;
    return true;
  }

  void print(raw_ostream &OS) const {
    if (EMUL) {
      OS << "EMUL: m";
      if (EMUL->second)
        OS << "f";
      OS << EMUL->first;
    } else
      OS << "EMUL: none\n";
    OS << ", EEW: " << (1 << Log2EEW);
  }
};

} // end anonymous namespace

char RVTrimVLOptimizerLegacy::ID = 0;
INITIALIZE_PASS_BEGIN(RVTrimVLOptimizerLegacy, DEBUG_TYPE, PASS_NAME, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_END(RVTrimVLOptimizerLegacy, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createRVTrimVLOptimizerLegacyPass() {
  return new RVTrimVLOptimizerLegacy();
}

[[maybe_unused]]
static raw_ostream &operator<<(raw_ostream &OS, const OperandInfo &OI) {
  OI.print(OS);
  return OS;
}

[[maybe_unused]]
static raw_ostream &operator<<(raw_ostream &OS,
                               const std::optional<OperandInfo> &OI) {
  if (OI)
    OI->print(OS);
  else
    OS << "nullopt";
  return OS;
}

/// Return EMUL = (EEW / SEW) * LMUL where EEW comes from Log2EEW and LMUL and
/// SEW are from the TSFlags of MI.
static std::pair<unsigned, bool>
getEMULEqualsEEWDivSEWTimesLMUL(unsigned Log2EEW, const MachineInstr &MI) {
  RISCVVType::VLMUL MIVLMUL = RVTrimII::getLMul(MI.getDesc().TSFlags);
  auto [MILMUL, MILMULIsFractional] = RISCVVType::decodeVLMUL(MIVLMUL);
  unsigned MILog2SEW =
      MI.getOperand(RVTrimII::getSEWOpNum(MI.getDesc())).getImm();

  // Mask instructions will have 0 as the SEW operand. But the LMUL of these
  // instructions is calculated is as if the SEW operand was 3 (e8).
  if (MILog2SEW == 0)
    MILog2SEW = 3;

  unsigned MISEW = 1 << MILog2SEW;

  unsigned EEW = 1 << Log2EEW;
  // Calculate (EEW/SEW)*LMUL preserving fractions less than 1. Use GCD
  // to put fraction in simplest form.
  unsigned Num = EEW, Denom = MISEW;
  int GCD = MILMULIsFractional ? std::gcd(Num, Denom * MILMUL)
                               : std::gcd(Num * MILMUL, Denom);
  Num = MILMULIsFractional ? Num / GCD : Num * MILMUL / GCD;
  Denom = MILMULIsFractional ? Denom * MILMUL / GCD : Denom / GCD;
  return std::make_pair(Num > Denom ? Num : Denom, Denom > Num);
}

static DemandedVL doubleVL(DemandedVL MinimumVL) {
  if (!MinimumVL.VL.isImm())
    return DemandedVL::vlmax();

  int64_t VL = MinimumVL.VL.getImm();
  if (!isUInt<4>(VL))
    return DemandedVL::vlmax();
  return MachineOperand::CreateImm(VL * 2);
}

static DemandedVL halfVL(DemandedVL MinimumVL, bool Ceil = false) {
  if (!MinimumVL.VL.isImm())
    return DemandedVL::vlmax();

  int64_t VL = MinimumVL.VL.getImm();
  if (!isUInt<5>(VL))
    return DemandedVL::vlmax();
  return MachineOperand::CreateImm((VL + Ceil) / 2);
}

static std::pair<unsigned, bool> doubleEMUL(std::pair<unsigned, bool> EMUL) {
  auto [Num, IsFractional] = EMUL;
  if (IsFractional)
    return std::make_pair(Num / 2, Num > 2);
  return std::make_pair(Num * 2, false);
}

static std::pair<unsigned, bool> halfEMUL(std::pair<unsigned, bool> EMUL) {
  auto [Num, IsFractional] = EMUL;
  if (IsFractional || Num == 1)
    return std::make_pair(Num * 2, true);
  return std::make_pair(Num / 2, false);
}

/// Dest has EEW=SEW. Source EEW=SEW/Factor (i.e. F2 => EEW/2).
/// SEW comes from TSFlags of MI.
static unsigned getIntegerExtensionOperandEEW(unsigned Factor,
                                              const MachineInstr &MI,
                                              unsigned OpIdx) {
  unsigned MILog2SEW =
      MI.getOperand(RVTrimII::getSEWOpNum(MI.getDesc())).getImm();

  if (OpIdx == 0)
    return MILog2SEW;

  unsigned MISEW = 1 << MILog2SEW;
  unsigned EEW = MISEW / Factor;
  unsigned Log2EEW = Log2_32(EEW);

  return Log2EEW;
}

#define VSEG_CASES(Prefix, EEW)                                                \
  RVTrim::Prefix##SEG2E##EEW##_V:                                               \
  case RVTrim::Prefix##SEG3E##EEW##_V:                                          \
  case RVTrim::Prefix##SEG4E##EEW##_V:                                          \
  case RVTrim::Prefix##SEG5E##EEW##_V:                                          \
  case RVTrim::Prefix##SEG6E##EEW##_V:                                          \
  case RVTrim::Prefix##SEG7E##EEW##_V:                                          \
  case RVTrim::Prefix##SEG8E##EEW##_V
#define VSSEG_CASES(EEW)    VSEG_CASES(VS, EEW)
#define VSSSEG_CASES(EEW)   VSEG_CASES(VSS, EEW)
#define VSUXSEG_CASES(EEW)  VSEG_CASES(VSUX, I##EEW)
#define VSOXSEG_CASES(EEW)  VSEG_CASES(VSOX, I##EEW)

static std::optional<unsigned> getOperandLog2EEW(const MachineInstr &MI,
                                                 unsigned OpIdx) {
  const MCInstrDesc &Desc = MI.getDesc();
  const RVTrimVPseudosTable::PseudoInfo *RVV =
      RVTrimVPseudosTable::getPseudoInfo(MI.getOpcode());
  assert(RVV && "Could not find MI in PseudoTable");

  // MI has a SEW associated with it. The RVV specification defines
  // the EEW of each operand and definition in relation to MI.SEW.
  unsigned MILog2SEW = MI.getOperand(RVTrimII::getSEWOpNum(Desc)).getImm();

  const bool HasPassthru = RVTrimII::isFirstDefTiedToFirstUse(Desc);
  const bool IsTied = RVTrimII::isTiedPseudo(Desc.TSFlags);

  bool IsMODef =
      OpIdx == 0 || (HasPassthru && OpIdx == MI.getNumExplicitDefs());

  // All mask operands have EEW=1
  const MCOperandInfo &Info = Desc.operands()[OpIdx];
  if (Info.OperandType == MCOI::OPERAND_REGISTER &&
      Info.RegClass == RVTrim::VMV0RegClassID)
    return 0;

  // switch against BaseInstr to reduce number of cases that need to be
  // considered.
  switch (RVV->BaseInstr) {

  // 6. Configuration-Setting Instructions
  // Configuration setting instructions do not read or write vector registers
  case RVTrim::VSETIVLI:
  case RVTrim::VSETVL:
  case RVTrim::VSETVLI:
    llvm_unreachable("Configuration setting instructions do not read or write "
                     "vector registers");

  // Vector Loads and Stores
  // Vector Unit-Stride Instructions
  // Vector Strided Instructions
  /// Dest EEW encoded in the instruction
  case RVTrim::VLM_V:
  case RVTrim::VSM_V:
    return 0;
  case RVTrim::VLE8_V:
  case RVTrim::VSE8_V:
  case RVTrim::VLSE8_V:
  case RVTrim::VSSE8_V:
  case VSSEG_CASES(8):
  case VSSSEG_CASES(8):
    return 3;
  case RVTrim::VLE16_V:
  case RVTrim::VSE16_V:
  case RVTrim::VLSE16_V:
  case RVTrim::VSSE16_V:
  case VSSEG_CASES(16):
  case VSSSEG_CASES(16):
    return 4;
  case RVTrim::VLE32_V:
  case RVTrim::VSE32_V:
  case RVTrim::VLSE32_V:
  case RVTrim::VSSE32_V:
  case VSSEG_CASES(32):
  case VSSSEG_CASES(32):
    return 5;
  case RVTrim::VLE64_V:
  case RVTrim::VSE64_V:
  case RVTrim::VLSE64_V:
  case RVTrim::VSSE64_V:
  case VSSEG_CASES(64):
  case VSSSEG_CASES(64):
    return 6;

  // Vector Indexed Instructions
  // vs(o|u)xei<eew>.v
  // Dest/Data (operand 0) EEW=SEW.  Source EEW=<eew>.
  case RVTrim::VLUXEI8_V:
  case RVTrim::VLOXEI8_V:
  case RVTrim::VSUXEI8_V:
  case RVTrim::VSOXEI8_V:
  case VSUXSEG_CASES(8):
  case VSOXSEG_CASES(8): {
    if (OpIdx == 0)
      return MILog2SEW;
    return 3;
  }
  case RVTrim::VLUXEI16_V:
  case RVTrim::VLOXEI16_V:
  case RVTrim::VSUXEI16_V:
  case RVTrim::VSOXEI16_V:
  case VSUXSEG_CASES(16):
  case VSOXSEG_CASES(16): {
    if (OpIdx == 0)
      return MILog2SEW;
    return 4;
  }
  case RVTrim::VLUXEI32_V:
  case RVTrim::VLOXEI32_V:
  case RVTrim::VSUXEI32_V:
  case RVTrim::VSOXEI32_V:
  case VSUXSEG_CASES(32):
  case VSOXSEG_CASES(32): {
    if (OpIdx == 0)
      return MILog2SEW;
    return 5;
  }
  case RVTrim::VLUXEI64_V:
  case RVTrim::VLOXEI64_V:
  case RVTrim::VSUXEI64_V:
  case RVTrim::VSOXEI64_V:
  case VSUXSEG_CASES(64):
  case VSOXSEG_CASES(64): {
    if (OpIdx == 0)
      return MILog2SEW;
    return 6;
  }

  // Vector Integer Arithmetic Instructions
  // Vector Single-Width Integer Add and Subtract
  case RVTrim::VADD_VI:
  case RVTrim::VADD_VV:
  case RVTrim::VADD_VX:
  case RVTrim::VSUB_VV:
  case RVTrim::VSUB_VX:
  case RVTrim::VRSUB_VI:
  case RVTrim::VRSUB_VX:
  // Vector Bitwise Logical Instructions
  // Vector Single-Width Shift Instructions
  // EEW=SEW.
  case RVTrim::VAND_VI:
  case RVTrim::VAND_VV:
  case RVTrim::VAND_VX:
  case RVTrim::VOR_VI:
  case RVTrim::VOR_VV:
  case RVTrim::VOR_VX:
  case RVTrim::VXOR_VI:
  case RVTrim::VXOR_VV:
  case RVTrim::VXOR_VX:
  case RVTrim::VSLL_VI:
  case RVTrim::VSLL_VV:
  case RVTrim::VSLL_VX:
  case RVTrim::VSRL_VI:
  case RVTrim::VSRL_VV:
  case RVTrim::VSRL_VX:
  case RVTrim::VSRA_VI:
  case RVTrim::VSRA_VV:
  case RVTrim::VSRA_VX:
  // Vector Integer Min/Max Instructions
  // EEW=SEW.
  case RVTrim::VMINU_VV:
  case RVTrim::VMINU_VX:
  case RVTrim::VMIN_VV:
  case RVTrim::VMIN_VX:
  case RVTrim::VMAXU_VV:
  case RVTrim::VMAXU_VX:
  case RVTrim::VMAX_VV:
  case RVTrim::VMAX_VX:
  // Vector Single-Width Integer Multiply Instructions
  // Source and Dest EEW=SEW.
  case RVTrim::VMUL_VV:
  case RVTrim::VMUL_VX:
  case RVTrim::VMULH_VV:
  case RVTrim::VMULH_VX:
  case RVTrim::VMULHU_VV:
  case RVTrim::VMULHU_VX:
  case RVTrim::VMULHSU_VV:
  case RVTrim::VMULHSU_VX:
  // Vector Integer Divide Instructions
  // EEW=SEW.
  case RVTrim::VDIVU_VV:
  case RVTrim::VDIVU_VX:
  case RVTrim::VDIV_VV:
  case RVTrim::VDIV_VX:
  case RVTrim::VREMU_VV:
  case RVTrim::VREMU_VX:
  case RVTrim::VREM_VV:
  case RVTrim::VREM_VX:
  // Vector Single-Width Integer Multiply-Add Instructions
  // EEW=SEW.
  case RVTrim::VMACC_VV:
  case RVTrim::VMACC_VX:
  case RVTrim::VNMSAC_VV:
  case RVTrim::VNMSAC_VX:
  case RVTrim::VMADD_VV:
  case RVTrim::VMADD_VX:
  case RVTrim::VNMSUB_VV:
  case RVTrim::VNMSUB_VX:
  // Vector Integer Merge Instructions
  // Vector Integer Add-with-Carry / Subtract-with-Borrow Instructions
  // EEW=SEW, except the mask operand has EEW=1. Mask operand is handled
  // before this switch.
  case RVTrim::VMERGE_VIM:
  case RVTrim::VMERGE_VVM:
  case RVTrim::VMERGE_VXM:
  case RVTrim::VADC_VIM:
  case RVTrim::VADC_VVM:
  case RVTrim::VADC_VXM:
  case RVTrim::VSBC_VVM:
  case RVTrim::VSBC_VXM:
  // Vector Integer Move Instructions
  // Vector Fixed-Point Arithmetic Instructions
  // Vector Single-Width Saturating Add and Subtract
  // Vector Single-Width Averaging Add and Subtract
  // EEW=SEW.
  case RVTrim::VMV_V_I:
  case RVTrim::VMV_V_V:
  case RVTrim::VMV_V_X:
  case RVTrim::VSADDU_VI:
  case RVTrim::VSADDU_VV:
  case RVTrim::VSADDU_VX:
  case RVTrim::VSADD_VI:
  case RVTrim::VSADD_VV:
  case RVTrim::VSADD_VX:
  case RVTrim::VSSUBU_VV:
  case RVTrim::VSSUBU_VX:
  case RVTrim::VSSUB_VV:
  case RVTrim::VSSUB_VX:
  case RVTrim::VAADDU_VV:
  case RVTrim::VAADDU_VX:
  case RVTrim::VAADD_VV:
  case RVTrim::VAADD_VX:
  case RVTrim::VASUBU_VV:
  case RVTrim::VASUBU_VX:
  case RVTrim::VASUB_VV:
  case RVTrim::VASUB_VX:
  // Vector Single-Width Fractional Multiply with Rounding and Saturation
  // EEW=SEW. The instruction produces 2*SEW product internally but
  // saturates to fit into SEW bits.
  case RVTrim::VSMUL_VV:
  case RVTrim::VSMUL_VX:
  // Vector Single-Width Scaling Shift Instructions
  // EEW=SEW.
  case RVTrim::VSSRL_VI:
  case RVTrim::VSSRL_VV:
  case RVTrim::VSSRL_VX:
  case RVTrim::VSSRA_VI:
  case RVTrim::VSSRA_VV:
  case RVTrim::VSSRA_VX:
  // Vector Permutation Instructions
  // Integer Scalar Move Instructions
  // Floating-Point Scalar Move Instructions
  // EEW=SEW.
  case RVTrim::VMV_X_S:
  case RVTrim::VMV_S_X:
  case RVTrim::VFMV_F_S:
  case RVTrim::VFMV_S_F:
  // Vector Slide Instructions
  // EEW=SEW.
  case RVTrim::VSLIDEUP_VI:
  case RVTrim::VSLIDEUP_VX:
  case RVTrim::VSLIDEDOWN_VI:
  case RVTrim::VSLIDEDOWN_VX:
  case RVTrim::VSLIDE1UP_VX:
  case RVTrim::VFSLIDE1UP_VF:
  case RVTrim::VSLIDE1DOWN_VX:
  case RVTrim::VFSLIDE1DOWN_VF:
  // Vector Register Gather Instructions
  // EEW=SEW. For mask operand, EEW=1.
  case RVTrim::VRGATHER_VI:
  case RVTrim::VRGATHER_VV:
  case RVTrim::VRGATHER_VX:
  // Vector Element Index Instruction
  case RVTrim::VID_V:
  // Vector Single-Width Floating-Point Add/Subtract Instructions
  case RVTrim::VFADD_VF:
  case RVTrim::VFADD_VV:
  case RVTrim::VFSUB_VF:
  case RVTrim::VFSUB_VV:
  case RVTrim::VFRSUB_VF:
  // Vector Single-Width Floating-Point Multiply/Divide Instructions
  case RVTrim::VFMUL_VF:
  case RVTrim::VFMUL_VV:
  case RVTrim::VFDIV_VF:
  case RVTrim::VFDIV_VV:
  case RVTrim::VFRDIV_VF:
  // Vector Single-Width Floating-Point Fused Multiply-Add Instructions
  case RVTrim::VFMACC_VV:
  case RVTrim::VFMACC_VF:
  case RVTrim::VFNMACC_VV:
  case RVTrim::VFNMACC_VF:
  case RVTrim::VFMSAC_VV:
  case RVTrim::VFMSAC_VF:
  case RVTrim::VFNMSAC_VV:
  case RVTrim::VFNMSAC_VF:
  case RVTrim::VFMADD_VV:
  case RVTrim::VFMADD_VF:
  case RVTrim::VFNMADD_VV:
  case RVTrim::VFNMADD_VF:
  case RVTrim::VFMSUB_VV:
  case RVTrim::VFMSUB_VF:
  case RVTrim::VFNMSUB_VV:
  case RVTrim::VFNMSUB_VF:
  // Vector Floating-Point Square-Root Instruction
  case RVTrim::VFSQRT_V:
  // Vector Floating-Point Reciprocal Square-Root Estimate Instruction
  case RVTrim::VFRSQRT7_V:
  // Vector Floating-Point Reciprocal Estimate Instruction
  case RVTrim::VFREC7_V:
  // Vector Floating-Point MIN/MAX Instructions
  case RVTrim::VFMIN_VF:
  case RVTrim::VFMIN_VV:
  case RVTrim::VFMAX_VF:
  case RVTrim::VFMAX_VV:
  // Vector Floating-Point Sign-Injection Instructions
  case RVTrim::VFSGNJ_VF:
  case RVTrim::VFSGNJ_VV:
  case RVTrim::VFSGNJN_VV:
  case RVTrim::VFSGNJN_VF:
  case RVTrim::VFSGNJX_VF:
  case RVTrim::VFSGNJX_VV:
  // Vector Floating-Point Classify Instruction
  case RVTrim::VFCLASS_V:
  // Vector Floating-Point Move Instruction
  case RVTrim::VFMV_V_F:
  // Single-Width Floating-Point/Integer Type-Convert Instructions
  case RVTrim::VFCVT_XU_F_V:
  case RVTrim::VFCVT_X_F_V:
  case RVTrim::VFCVT_RTZ_XU_F_V:
  case RVTrim::VFCVT_RTZ_X_F_V:
  case RVTrim::VFCVT_F_XU_V:
  case RVTrim::VFCVT_F_X_V:
  // Vector Floating-Point Merge Instruction
  case RVTrim::VFMERGE_VFM:
  // Vector count population in mask vcpop.m
  // vfirst find-first-set mask bit
  case RVTrim::VCPOP_M:
  case RVTrim::VFIRST_M:
  // Vector Bit-manipulation Instructions (Zvbb)
  // Vector And-Not
  case RVTrim::VANDN_VV:
  case RVTrim::VANDN_VX:
  // Vector Reverse Bits in Elements
  case RVTrim::VBREV_V:
  // Vector Reverse Bits in Bytes
  case RVTrim::VBREV8_V:
  // Vector Reverse Bytes
  case RVTrim::VREV8_V:
  // Vector Count Leading Zeros
  case RVTrim::VCLZ_V:
  // Vector Count Trailing Zeros
  case RVTrim::VCTZ_V:
  // Vector Population Count
  case RVTrim::VCPOP_V:
  // Vector Rotate Left
  case RVTrim::VROL_VV:
  case RVTrim::VROL_VX:
  // Vector Rotate Right
  case RVTrim::VROR_VI:
  case RVTrim::VROR_VV:
  case RVTrim::VROR_VX:
  // Vector Carry-less Multiplication Instructions (Zvbc)
  // Vector Carry-less Multiply
  case RVTrim::VCLMUL_VV:
  case RVTrim::VCLMUL_VX:
  // Vector Carry-less Multiply Return High Half
  case RVTrim::VCLMULH_VV:
  case RVTrim::VCLMULH_VX:

  // Zvabd
  case RVTrim::VABD_VV:
  case RVTrim::VABD_VX:
  case RVTrim::VABDU_VV:
  case RVTrim::VABDU_VX:

  // Zvzip
  case RVTrim::VZIP_VV:
  case RVTrim::VUNZIPE_V:
  case RVTrim::VUNZIPO_V:
  case RVTrim::VPAIRE_VV:
  case RVTrim::VPAIRO_VV:
    return MILog2SEW;

  // Vector Widening Shift Left Logical (Zvbb)
  case RVTrim::VWSLL_VI:
  case RVTrim::VWSLL_VX:
  case RVTrim::VWSLL_VV:
  // Vector Widening Integer Add/Subtract
  // Def uses EEW=2*SEW . Operands use EEW=SEW.
  case RVTrim::VWADDU_VV:
  case RVTrim::VWADDU_VX:
  case RVTrim::VWSUBU_VV:
  case RVTrim::VWSUBU_VX:
  case RVTrim::VWADD_VV:
  case RVTrim::VWADD_VX:
  case RVTrim::VWSUB_VV:
  case RVTrim::VWSUB_VX:
  // Vector Widening Integer Multiply Instructions
  // Destination EEW=2*SEW. Source EEW=SEW.
  case RVTrim::VWMUL_VV:
  case RVTrim::VWMUL_VX:
  case RVTrim::VWMULSU_VV:
  case RVTrim::VWMULSU_VX:
  case RVTrim::VWMULU_VV:
  case RVTrim::VWMULU_VX:
  // Vector Widening Integer Multiply-Add Instructions
  // Destination EEW=2*SEW. Source EEW=SEW.
  // A SEW-bit*SEW-bit multiply of the sources forms a 2*SEW-bit value, which
  // is then added to the 2*SEW-bit Dest. These instructions never have a
  // passthru operand.
  case RVTrim::VWMACCU_VV:
  case RVTrim::VWMACCU_VX:
  case RVTrim::VWMACC_VV:
  case RVTrim::VWMACC_VX:
  case RVTrim::VWMACCSU_VV:
  case RVTrim::VWMACCSU_VX:
  case RVTrim::VWMACCUS_VX:
  // Vector Widening Floating-Point Fused Multiply-Add Instructions
  case RVTrim::VFWMACC_VF:
  case RVTrim::VFWMACC_VV:
  case RVTrim::VFWNMACC_VF:
  case RVTrim::VFWNMACC_VV:
  case RVTrim::VFWMSAC_VF:
  case RVTrim::VFWMSAC_VV:
  case RVTrim::VFWNMSAC_VF:
  case RVTrim::VFWNMSAC_VV:
  case RVTrim::VFWMACCBF16_VV:
  case RVTrim::VFWMACCBF16_VF:
  // Vector Widening Floating-Point Add/Subtract Instructions
  // Dest EEW=2*SEW. Source EEW=SEW.
  case RVTrim::VFWADD_VV:
  case RVTrim::VFWADD_VF:
  case RVTrim::VFWSUB_VV:
  case RVTrim::VFWSUB_VF:
  // Vector Widening Floating-Point Multiply
  case RVTrim::VFWMUL_VF:
  case RVTrim::VFWMUL_VV:
  // Widening Floating-Point/Integer Type-Convert Instructions
  case RVTrim::VFWCVT_XU_F_V:
  case RVTrim::VFWCVT_X_F_V:
  case RVTrim::VFWCVT_RTZ_XU_F_V:
  case RVTrim::VFWCVT_RTZ_X_F_V:
  case RVTrim::VFWCVT_F_XU_V:
  case RVTrim::VFWCVT_F_X_V:
  case RVTrim::VFWCVT_F_F_V:
  case RVTrim::VFWCVTBF16_F_F_V:
  // Zvabd
  case RVTrim::VWABDA_VV:
  case RVTrim::VWABDA_VX:
  case RVTrim::VWABDAU_VV:
  case RVTrim::VWABDAU_VX:
    return IsMODef ? MILog2SEW + 1 : MILog2SEW;

  // Def and Op1 uses EEW=2*SEW. Op2 uses EEW=SEW.
  case RVTrim::VWADDU_WV:
  case RVTrim::VWADDU_WX:
  case RVTrim::VWSUBU_WV:
  case RVTrim::VWSUBU_WX:
  case RVTrim::VWADD_WV:
  case RVTrim::VWADD_WX:
  case RVTrim::VWSUB_WV:
  case RVTrim::VWSUB_WX:
  // Vector Widening Floating-Point Add/Subtract Instructions
  case RVTrim::VFWADD_WF:
  case RVTrim::VFWADD_WV:
  case RVTrim::VFWSUB_WF:
  case RVTrim::VFWSUB_WV: {
    bool IsOp1 = (HasPassthru && !IsTied) ? OpIdx == 2 : OpIdx == 1;
    bool TwoTimes = IsMODef || IsOp1;
    return TwoTimes ? MILog2SEW + 1 : MILog2SEW;
  }

  // Vector Integer Extension
  case RVTrim::VZEXT_VF2:
  case RVTrim::VSEXT_VF2:
    return getIntegerExtensionOperandEEW(2, MI, OpIdx);
  case RVTrim::VZEXT_VF4:
  case RVTrim::VSEXT_VF4:
    return getIntegerExtensionOperandEEW(4, MI, OpIdx);
  case RVTrim::VZEXT_VF8:
  case RVTrim::VSEXT_VF8:
    return getIntegerExtensionOperandEEW(8, MI, OpIdx);

  // Vector Narrowing Integer Right Shift Instructions
  // Destination EEW=SEW, Op 1 has EEW=2*SEW. Op2 has EEW=SEW
  case RVTrim::VNSRL_WX:
  case RVTrim::VNSRL_WI:
  case RVTrim::VNSRL_WV:
  case RVTrim::VNSRA_WI:
  case RVTrim::VNSRA_WV:
  case RVTrim::VNSRA_WX:
  // Vector Narrowing Fixed-Point Clip Instructions
  // Destination and Op1 EEW=SEW. Op2 EEW=2*SEW.
  case RVTrim::VNCLIPU_WI:
  case RVTrim::VNCLIPU_WV:
  case RVTrim::VNCLIPU_WX:
  case RVTrim::VNCLIP_WI:
  case RVTrim::VNCLIP_WV:
  case RVTrim::VNCLIP_WX:
  // Narrowing Floating-Point/Integer Type-Convert Instructions
  case RVTrim::VFNCVT_XU_F_W:
  case RVTrim::VFNCVT_X_F_W:
  case RVTrim::VFNCVT_RTZ_XU_F_W:
  case RVTrim::VFNCVT_RTZ_X_F_W:
  case RVTrim::VFNCVT_F_XU_W:
  case RVTrim::VFNCVT_F_X_W:
  case RVTrim::VFNCVT_F_F_W:
  case RVTrim::VFNCVT_ROD_F_F_W:
  case RVTrim::VFNCVTBF16_F_F_W: {
    assert(!IsTied);
    bool IsOp1 = HasPassthru ? OpIdx == 2 : OpIdx == 1;
    bool TwoTimes = IsOp1;
    return TwoTimes ? MILog2SEW + 1 : MILog2SEW;
  }

  // Vector Mask Instructions
  // Vector Mask-Register Logical Instructions
  // vmsbf.m set-before-first mask bit
  // vmsif.m set-including-first mask bit
  // vmsof.m set-only-first mask bit
  // EEW=1
  // We handle the cases when operand is a v0 mask operand above the switch,
  // but these instructions may use non-v0 mask operands and need to be handled
  // specifically.
  case RVTrim::VMAND_MM:
  case RVTrim::VMNAND_MM:
  case RVTrim::VMANDN_MM:
  case RVTrim::VMXOR_MM:
  case RVTrim::VMOR_MM:
  case RVTrim::VMNOR_MM:
  case RVTrim::VMORN_MM:
  case RVTrim::VMXNOR_MM:
  case RVTrim::VMSBF_M:
  case RVTrim::VMSIF_M:
  case RVTrim::VMSOF_M: {
    return MILog2SEW;
  }

  // Vector Compress Instruction
  // EEW=SEW, except the mask operand has EEW=1. Mask operand is not handled
  // before this switch.
  case RVTrim::VCOMPRESS_VM:
    return OpIdx == 3 ? 0 : MILog2SEW;

  // Vector Iota Instruction
  // EEW=SEW, except the mask operand has EEW=1. Mask operand is not handled
  // before this switch.
  case RVTrim::VIOTA_M: {
    if (IsMODef || OpIdx == 1)
      return MILog2SEW;
    return 0;
  }

  // Vector Integer Compare Instructions
  // Dest EEW=1. Source EEW=SEW.
  case RVTrim::VMSEQ_VI:
  case RVTrim::VMSEQ_VV:
  case RVTrim::VMSEQ_VX:
  case RVTrim::VMSNE_VI:
  case RVTrim::VMSNE_VV:
  case RVTrim::VMSNE_VX:
  case RVTrim::VMSLTU_VV:
  case RVTrim::VMSLTU_VX:
  case RVTrim::VMSLT_VV:
  case RVTrim::VMSLT_VX:
  case RVTrim::VMSLEU_VV:
  case RVTrim::VMSLEU_VI:
  case RVTrim::VMSLEU_VX:
  case RVTrim::VMSLE_VV:
  case RVTrim::VMSLE_VI:
  case RVTrim::VMSLE_VX:
  case RVTrim::VMSGTU_VI:
  case RVTrim::VMSGTU_VX:
  case RVTrim::VMSGT_VI:
  case RVTrim::VMSGT_VX:
  // Vector Integer Add-with-Carry / Subtract-with-Borrow Instructions
  // Dest EEW=1. Source EEW=SEW. Mask source operand handled above this switch.
  case RVTrim::VMADC_VIM:
  case RVTrim::VMADC_VVM:
  case RVTrim::VMADC_VXM:
  case RVTrim::VMSBC_VVM:
  case RVTrim::VMSBC_VXM:
  // Dest EEW=1. Source EEW=SEW.
  case RVTrim::VMADC_VV:
  case RVTrim::VMADC_VI:
  case RVTrim::VMADC_VX:
  case RVTrim::VMSBC_VV:
  case RVTrim::VMSBC_VX:
  // 13.13. Vector Floating-Point Compare Instructions
  // Dest EEW=1. Source EEW=SEW
  case RVTrim::VMFEQ_VF:
  case RVTrim::VMFEQ_VV:
  case RVTrim::VMFNE_VF:
  case RVTrim::VMFNE_VV:
  case RVTrim::VMFLT_VF:
  case RVTrim::VMFLT_VV:
  case RVTrim::VMFLE_VF:
  case RVTrim::VMFLE_VV:
  case RVTrim::VMFGT_VF:
  case RVTrim::VMFGE_VF: {
    if (IsMODef)
      return 0;
    return MILog2SEW;
  }

  // Vector Reduction Operations
  // Vector Single-Width Integer Reduction Instructions
  case RVTrim::VREDAND_VS:
  case RVTrim::VREDMAX_VS:
  case RVTrim::VREDMAXU_VS:
  case RVTrim::VREDMIN_VS:
  case RVTrim::VREDMINU_VS:
  case RVTrim::VREDOR_VS:
  case RVTrim::VREDSUM_VS:
  case RVTrim::VREDXOR_VS:
  // Vector Single-Width Floating-Point Reduction Instructions
  case RVTrim::VFREDMAX_VS:
  case RVTrim::VFREDMIN_VS:
  case RVTrim::VFREDOSUM_VS:
  case RVTrim::VFREDUSUM_VS: {
    return MILog2SEW;
  }

  // Vector Widening Integer Reduction Instructions
  // The Dest and VS1 read only element 0 for the vector register. Return
  // 2*EEW for these. VS2 has EEW=SEW and EMUL=LMUL.
  case RVTrim::VWREDSUM_VS:
  case RVTrim::VWREDSUMU_VS:
  // Vector Widening Floating-Point Reduction Instructions
  case RVTrim::VFWREDOSUM_VS:
  case RVTrim::VFWREDUSUM_VS: {
    bool TwoTimes = IsMODef || OpIdx == 3;
    return TwoTimes ? MILog2SEW + 1 : MILog2SEW;
  }

  // Vector Register Gather with 16-bit Index Elements Instruction
  // Dest and source data EEW=SEW. Index vector EEW=16.
  case RVTrim::VRGATHEREI16_VV: {
    if (OpIdx == 2)
      return 4;
    return MILog2SEW;
  }

  default:
    return std::nullopt;
  }
}

static std::optional<OperandInfo> getOperandInfo(const MachineInstr &MI,
                                                 unsigned OpIdx) {
  const RVTrimVPseudosTable::PseudoInfo *RVV =
      RVTrimVPseudosTable::getPseudoInfo(MI.getOpcode());
  assert(RVV && "Could not find MI in PseudoTable");

  std::optional<unsigned> Log2EEW = getOperandLog2EEW(MI, OpIdx);
  if (!Log2EEW)
    return std::nullopt;

  switch (RVV->BaseInstr) {
  // Vector Reduction Operations
  // Vector Single-Width Integer Reduction Instructions
  // Vector Widening Integer Reduction Instructions
  // Vector Widening Floating-Point Reduction Instructions
  // The Dest and VS1 only read element 0 of the vector register. Return just
  // the EEW for these.
  case RVTrim::VREDAND_VS:
  case RVTrim::VREDMAX_VS:
  case RVTrim::VREDMAXU_VS:
  case RVTrim::VREDMIN_VS:
  case RVTrim::VREDMINU_VS:
  case RVTrim::VREDOR_VS:
  case RVTrim::VREDSUM_VS:
  case RVTrim::VREDXOR_VS:
  case RVTrim::VWREDSUM_VS:
  case RVTrim::VWREDSUMU_VS:
  case RVTrim::VFWREDOSUM_VS:
  case RVTrim::VFWREDUSUM_VS:
    if (OpIdx != 2)
      return OperandInfo(*Log2EEW);
    break;

  // Zvzip - vzip.vv interleaves two half-LMUL vectors into an LMUL result with
  // the same SEW. The vtype LMUL describes the result, so only the two source
  // operands have half the instruction's EMUL.
  case RVTrim::VZIP_VV: {
    auto EMUL = getEMULEqualsEEWDivSEWTimesLMUL(*Log2EEW, MI);
    if (OpIdx == 2 || OpIdx == 3)
      EMUL = halfEMUL(EMUL);
    return OperandInfo(EMUL, *Log2EEW);
  }
  // Zvzip - vunzipe.v / vunzipo.v split a 2*LMUL vector into LMUL even/odd
  // elements with the same SEW. The source (and passthru tied to dest which is
  // also LMUL sized - so only the vs2 source) has 2 * EMUL.
  case RVTrim::VUNZIPE_V:
  case RVTrim::VUNZIPO_V: {
    auto EMUL = getEMULEqualsEEWDivSEWTimesLMUL(*Log2EEW, MI);
    if (OpIdx == 2)
      EMUL = doubleEMUL(EMUL);
    return OperandInfo(EMUL, *Log2EEW);
  }
  };

  // All others have EMUL=EEW/SEW*LMUL
  return OperandInfo(getEMULEqualsEEWDivSEWTimesLMUL(*Log2EEW, MI), *Log2EEW);
}

static bool isTupleInsertInstr(const MachineInstr &MI);

/// Return true if we can reason about demanded VLs elementwise for \p MI.
bool RVTrimVLOptimizerImpl::isSupportedInstr(const MachineInstr &MI) const {
  if (MI.isPHI() || MI.isFullCopy() || isTupleInsertInstr(MI))
    return true;

  unsigned RVVOpc = RVTrim::getRVVMCOpcode(MI.getOpcode());
  if (!RVVOpc)
    return false;

  assert(!(MI.getNumExplicitDefs() == 0 && !MI.mayStore() &&
           !RVTrimII::elementsDependOnVL(TII->get(RVVOpc).TSFlags)) &&
         "No defs but elements don't depend on VL?");

  // TODO: Reduce vl for vmv.s.x and vfmv.s.f. Currently this introduces more vl
  // toggles, we need to extend PRE in RVTrimInsertVSETVLI first.
  if (RVVOpc == RVTrim::VMV_S_X || RVVOpc == RVTrim::VFMV_S_F)
    return false;

  if (RVTrimII::elementsDependOnVL(TII->get(RVVOpc).TSFlags))
    return false;

  if (MI.mayStore())
    return false;

  return true;
}

/// Return true if operand \p OpIdx of \p MI is a vector operand but is used as
/// a scalar operand.
static bool isVectorOpUsedAsScalarOp(const MachineInstr &MI, unsigned OpIdx) {
  const RVTrimVPseudosTable::PseudoInfo *RVV =
      RVTrimVPseudosTable::getPseudoInfo(MI.getOpcode());

  if (!RVV)
    return false;

  switch (RVV->BaseInstr) {
  // Reductions only use vs1[0] of vs1
  case RVTrim::VREDAND_VS:
  case RVTrim::VREDMAX_VS:
  case RVTrim::VREDMAXU_VS:
  case RVTrim::VREDMIN_VS:
  case RVTrim::VREDMINU_VS:
  case RVTrim::VREDOR_VS:
  case RVTrim::VREDSUM_VS:
  case RVTrim::VREDXOR_VS:
  case RVTrim::VWREDSUM_VS:
  case RVTrim::VWREDSUMU_VS:
  case RVTrim::VFREDMAX_VS:
  case RVTrim::VFREDMIN_VS:
  case RVTrim::VFREDOSUM_VS:
  case RVTrim::VFREDUSUM_VS:
  case RVTrim::VFWREDOSUM_VS:
  case RVTrim::VFWREDUSUM_VS:
    return OpIdx == 3;
  case RVTrim::VMV_X_S:
  case RVTrim::VFMV_F_S:
    return OpIdx == 1;
  default:
    return false;
  }
}

bool RVTrimVLOptimizerImpl::isCandidate(const MachineInstr &MI) const {
  const MCInstrDesc &Desc = MI.getDesc();
  if (!RVTrimII::hasVLOp(Desc.TSFlags) || !RVTrimII::hasSEWOp(Desc.TSFlags))
    return false;

  if (MI.getNumExplicitDefs() != 1)
    return false;

  // Some instructions have implicit defs e.g. $vxsat. If they might be read
  // later then we can't reduce VL.
  if (!MI.allImplicitDefsAreDead()) {
    LLVM_DEBUG(dbgs() << "Not a candidate because has non-dead implicit def\n");
    return false;
  }

  if (MI.mayRaiseFPException()) {
    LLVM_DEBUG(dbgs() << "Not a candidate because may raise FP exception\n");
    return false;
  }

  for (const MachineMemOperand *MMO : MI.memoperands()) {
    if (MMO->isVolatile()) {
      LLVM_DEBUG(dbgs() << "Not a candidate because contains volatile MMO\n");
      return false;
    }
  }

  if (!isSupportedInstr(MI)) {
    LLVM_DEBUG(dbgs() << "Not a candidate due to unsupported instruction: "
                      << MI);
    return false;
  }

  assert(!RVTrimII::elementsDependOnVL(
             TII->get(RVTrim::getRVVMCOpcode(MI.getOpcode())).TSFlags) &&
         "Instruction shouldn't be supported if elements depend on VL");

  assert(RVTrimRI::isVRegClass(
             MRI->getRegClass(MI.getOperand(0).getReg())->TSFlags) &&
         "All supported instructions produce a vector register result");

  LLVM_DEBUG(dbgs() << "Found a candidate for VL reduction: " << MI << "\n");
  return true;
}

/// Given a vslidedown.vx like:
///
/// %slideamt = ADDI %x, -1
/// %v = PseudoVSLIDEDOWN_VX %passthru, %src, %slideamt, avl=1
///
/// %v will only read the first %slideamt + 1 lanes of %src, which = %x.
/// This is a common case when lowering extractelement.
///
/// Note that if %x is 0, %slideamt will be all ones. In this case %src will be
/// completely slid down and none of its lanes will be read (since %slideamt is
/// greater than the largest VLMAX of 65536) so we can demand any minimum VL.
static std::optional<DemandedVL>
getMinimumVLForVSLIDEDOWN_VX(const MachineInstr &MI, unsigned OpIdx,
                             const MachineRegisterInfo *MRI) {
  if (RVTrim::getRVVMCOpcode(MI.getOpcode()) != RVTrim::VSLIDEDOWN_VX)
    return std::nullopt;
  // We're looking at what lanes are used from the src operand.
  if (OpIdx != 2)
    return std::nullopt;
  // For now, the AVL must be 1.
  const MachineOperand &AVL = MI.getOperand(4);
  if (!AVL.isImm() || AVL.getImm() != 1)
    return std::nullopt;
  // The slide amount must be %x - 1.
  const MachineOperand &SlideAmt = MI.getOperand(3);
  if (!SlideAmt.getReg().isVirtual())
    return std::nullopt;
  MachineInstr *SlideAmtDef = MRI->getVRegDef(SlideAmt.getReg());
  if (!SlideAmtDef || SlideAmtDef->getOpcode() != RVTrim::ADDI ||
      SlideAmtDef->getOperand(2).getImm() != -AVL.getImm() ||
      !SlideAmtDef->getOperand(1).getReg().isVirtual())
    return std::nullopt;
  return SlideAmtDef->getOperand(1);
}

DemandedVL RVTrimVLOptimizerImpl::getMinimumVLForUser(const MachineInstr &UserMI,
                                                     unsigned OpIdx) const {
  const MachineOperand &UserOp = UserMI.getOperand(OpIdx);
  const MCInstrDesc &Desc = UserMI.getDesc();

  if (UserMI.isPHI() || UserMI.isFullCopy() || isTupleInsertInstr(UserMI))
    return DemandedVLs.lookup(&UserMI);

  if (!RVTrimII::hasVLOp(Desc.TSFlags) || !RVTrimII::hasSEWOp(Desc.TSFlags)) {
    LLVM_DEBUG(dbgs() << "  Abort due to lack of VL, assume that"
                         " use VLMAX\n");
    return DemandedVL::vlmax();
  }

  if (auto VL = getMinimumVLForVSLIDEDOWN_VX(UserMI, OpIdx, MRI))
    return *VL;

  unsigned RVVOpc = RVTrim::getRVVMCOpcode(UserMI.getOpcode());
  bool IsVUNZIP = RVVOpc == RVTrim::VUNZIPE_V || RVVOpc == RVTrim::VUNZIPO_V;
  bool IsVZIP = RVVOpc == RVTrim::VZIP_VV;
  if (!IsVUNZIP && RVTrimII::readsPastVL(TII->get(RVVOpc).TSFlags)) {
    LLVM_DEBUG(dbgs() << "  Abort because used by unsafe instruction\n");
    return DemandedVL::vlmax();
  }

  unsigned VLOpNum = RVTrimII::getVLOpNum(Desc);
  const MachineOperand &VLOp = UserMI.getOperand(VLOpNum);
  // Looking for an immediate or a register VL that isn't X0.
  assert((!VLOp.isReg() || VLOp.getReg() != RVTrim::X0) &&
         "Did not expect X0 VL");

  // If the user is a passthru it will read the elements past VL, so
  // abort if any of the elements past VL are demanded.
  if (UserOp.isTied()) {
    assert(OpIdx == UserMI.getNumExplicitDefs() &&
           RVTrimII::isFirstDefTiedToFirstUse(UserMI.getDesc()));
    if (!RVTrim::isVLKnownLE(*MRI, DemandedVLs.lookup(&UserMI).VL, VLOp)) {
      LLVM_DEBUG(dbgs() << "  Abort because user is passthru in "
                           "instruction with demanded tail\n");
      return DemandedVL::vlmax();
    }
  }

  // Instructions like reductions may use a vector register as a scalar
  // register. In this case, we should treat it as only reading the first lane.
  if (isVectorOpUsedAsScalarOp(UserMI, OpIdx)) {
    LLVM_DEBUG(dbgs() << "    Used this operand as a scalar operand\n");
    return MachineOperand::CreateImm(1);
  }

  // If we know the demanded VL of UserMI, then we can reduce the VL it
  // requires.
  DemandedVL MinimumVL = VLOp;
  if (RVTrim::isVLKnownLE(*MRI, DemandedVLs.lookup(&UserMI).VL, VLOp))
    MinimumVL = DemandedVLs.lookup(&UserMI);

  if (IsVUNZIP && OpIdx == 2)
    MinimumVL = doubleVL(MinimumVL);
  if (IsVZIP && (OpIdx == 2 || OpIdx == 3))
    MinimumVL = halfVL(MinimumVL, OpIdx == 2);

  return MinimumVL;
}

/// Return true if MI is an instruction used for assembling registers
/// for segmented store instructions, namely, RVTrimISD::TUPLE_INSERT.
/// Currently it's lowered to INSERT_SUBREG.
static bool isTupleInsertInstr(const MachineInstr &MI) {
  if (!MI.isInsertSubreg())
    return false;

  const MachineRegisterInfo &MRI = MI.getMF()->getRegInfo();
  const TargetRegisterClass *DstRC = MRI.getRegClass(MI.getOperand(0).getReg());
  const TargetRegisterInfo *TRI = MRI.getTargetRegisterInfo();
  if (!RVTrimRI::isVRegClass(DstRC->TSFlags))
    return false;
  unsigned NF = RVTrimRI::getNF(DstRC->TSFlags);
  if (NF < 2)
    return false;

  // Check whether INSERT_SUBREG has the correct subreg index for tuple inserts.
  auto VLMul = RVTrimRI::getLMul(DstRC->TSFlags);
  unsigned SubRegIdx = MI.getOperand(3).getImm();
  [[maybe_unused]] auto [LMul, IsFractional] = RISCVVType::decodeVLMUL(VLMul);
  assert(!IsFractional && "unexpected LMUL for tuple register classes");
  return TRI->getSubRegIdxSize(SubRegIdx) == RISCV::RVVBitsPerBlock * LMul;
}

static bool isSegmentedStoreInstr(const MachineInstr &MI) {
  switch (RVTrim::getRVVMCOpcode(MI.getOpcode())) {
  case VSSEG_CASES(8):
  case VSSSEG_CASES(8):
  case VSUXSEG_CASES(8):
  case VSOXSEG_CASES(8):
  case VSSEG_CASES(16):
  case VSSSEG_CASES(16):
  case VSUXSEG_CASES(16):
  case VSOXSEG_CASES(16):
  case VSSEG_CASES(32):
  case VSSSEG_CASES(32):
  case VSUXSEG_CASES(32):
  case VSOXSEG_CASES(32):
  case VSSEG_CASES(64):
  case VSSSEG_CASES(64):
  case VSUXSEG_CASES(64):
  case VSOXSEG_CASES(64):
    return true;
  default:
    return false;
  }
}

bool RVTrimVLOptimizerImpl::checkUsers(const MachineInstr &MI) const {
  if (MI.isPHI() || MI.isFullCopy() || isTupleInsertInstr(MI))
    return true;

  SmallSetVector<MachineOperand *, 8> OpWorklist;
  SmallPtrSet<const MachineInstr *, 4> PHISeen;
  for (auto &UserOp : MRI->use_operands(MI.getOperand(0).getReg()))
    OpWorklist.insert(&UserOp);

  while (!OpWorklist.empty()) {
    MachineOperand &UserOp = *OpWorklist.pop_back_val();
    const MachineInstr &UserMI = *UserOp.getParent();
    LLVM_DEBUG(dbgs() << "  Checking user: " << UserMI << "\n");

    if (UserMI.isFullCopy() && UserMI.getOperand(0).getReg().isVirtual()) {
      LLVM_DEBUG(dbgs() << "    Peeking through uses of COPY\n");
      OpWorklist.insert_range(llvm::make_pointer_range(
          MRI->use_operands(UserMI.getOperand(0).getReg())));
      continue;
    }

    if (isTupleInsertInstr(UserMI)) {
      LLVM_DEBUG(dbgs().indent(4) << "Peeking through uses of INSERT_SUBREG\n");
      for (MachineOperand &UseOp :
           MRI->use_operands(UserMI.getOperand(0).getReg())) {
        const MachineInstr &CandidateMI = *UseOp.getParent();
        // We should not propagate the VL if the user is not a segmented store
        // or another INSERT_SUBREG, since VL just works differently
        // between segmented operations (per-field) v.s. other RVV ops (on the
        // whole register group).
        if (!isTupleInsertInstr(CandidateMI) &&
            !isSegmentedStoreInstr(CandidateMI))
          return false;
        OpWorklist.insert(&UseOp);
      }
      continue;
    }

    if (UserMI.isPHI()) {
      // Don't follow PHI cycles
      if (!PHISeen.insert(&UserMI).second)
        continue;
      LLVM_DEBUG(dbgs() << "    Peeking through uses of PHI\n");
      OpWorklist.insert_range(llvm::make_pointer_range(
          MRI->use_operands(UserMI.getOperand(0).getReg())));
      continue;
    }

    if (!RVTrimII::hasSEWOp(UserMI.getDesc().TSFlags)) {
      LLVM_DEBUG(dbgs() << "    Abort due to lack of SEW operand\n");
      return false;
    }

    std::optional<OperandInfo> ConsumerInfo =
        getOperandInfo(UserMI, UserMI.getOperandNo(&UserOp));
    std::optional<OperandInfo> ProducerInfo = getOperandInfo(MI, 0);
    if (!ConsumerInfo || !ProducerInfo) {
      LLVM_DEBUG(dbgs() << "    Abort due to unknown operand information.\n");
      LLVM_DEBUG(dbgs() << "      ConsumerInfo is: " << ConsumerInfo << "\n");
      LLVM_DEBUG(dbgs() << "      ProducerInfo is: " << ProducerInfo << "\n");
      return false;
    }

    if (!OperandInfo::areCompatible(*ProducerInfo, *ConsumerInfo)) {
      LLVM_DEBUG(
          dbgs()
          << "    Abort due to incompatible information for EMUL or EEW.\n");
      LLVM_DEBUG(dbgs() << "      ConsumerInfo is: " << ConsumerInfo << "\n");
      LLVM_DEBUG(dbgs() << "      ProducerInfo is: " << ProducerInfo << "\n");
      return false;
    }
  }

  return true;
}

bool RVTrimVLOptimizerImpl::tryReduceVL(MachineInstr &MI,
                                       MachineOperand CommonVL) {
  LLVM_DEBUG(dbgs() << "Trying to reduce VL for " << MI);

  unsigned VLOpNum = RVTrimII::getVLOpNum(MI.getDesc());
  MachineOperand &VLOp = MI.getOperand(VLOpNum);

  assert((CommonVL.isImm() || CommonVL.getReg().isVirtual()) &&
         "Expected VL to be an Imm or virtual Reg");

  // If the VL is defined by a vleff that doesn't dominate MI, try using the
  // vleff's AVL. It will be greater than or equal to the output VL.
  if (CommonVL.isReg()) {
    const MachineInstr *VLMI = MRI->getVRegDef(CommonVL.getReg());
    if (VLMI && RVTrimInstrInfo::isFaultOnlyFirstLoad(*VLMI)) {
      if (!MDT)
        MDT = &GetMDT();
      if (!MDT->dominates(VLMI, &MI))
        CommonVL = VLMI->getOperand(RVTrimII::getVLOpNum(VLMI->getDesc()));
    }
  }

  if (!RVTrim::isVLKnownLE(*MRI, CommonVL, VLOp)) {
    LLVM_DEBUG(dbgs() << "  Abort due to CommonVL not <= VLOp.\n");
    return false;
  }

  if (CommonVL.isIdenticalTo(VLOp)) {
    LLVM_DEBUG(
        dbgs() << "  Abort due to CommonVL == VLOp, no point in reducing.\n");
    return false;
  }

  if (CommonVL.isImm()) {
    LLVM_DEBUG(dbgs() << "  Reduce VL from " << VLOp << " to "
                      << CommonVL.getImm() << " for " << MI << "\n");
    VLOp.ChangeToImmediate(CommonVL.getImm());
    return true;
  }
  MachineInstr *VLMI = MRI->getVRegDef(CommonVL.getReg());
  if (!VLMI)
    return false;

  if (!MDT)
    MDT = &GetMDT();
  auto VLDominates = [this, &VLMI](const MachineInstr &MI) {
    return MDT->dominates(VLMI, &MI);
  };
  if (!VLDominates(MI)) {
    assert(MI.getNumExplicitDefs() == 1);
    auto Uses = MRI->use_instructions(MI.getOperand(0).getReg());
    auto UsesSameBB = make_filter_range(Uses, [&MI](const MachineInstr &Use) {
      return Use.getParent() == MI.getParent();
    });
    if (VLMI->getParent() == MI.getParent() &&
        all_of(UsesSameBB, VLDominates) &&
        RVTrimInstrInfo::isSafeToMove(MI, std::next(VLMI->getIterator()))) {
      VLMI->getParent()->splice(std::next(VLMI->getIterator()), MI.getParent(),
                                MI.getIterator());
    } else {
      LLVM_DEBUG(dbgs() << "  Abort due to VL not dominating.\n");
      return false;
    }
  }
  LLVM_DEBUG(dbgs() << "  Reduce VL from " << VLOp << " to "
                    << printReg(CommonVL.getReg(), MRI->getTargetRegisterInfo())
                    << " for " << MI << "\n");

  // All our checks passed. We can reduce VL.
  VLOp.ChangeToRegister(CommonVL.getReg(), false);
  MRI->constrainRegClass(CommonVL.getReg(), &RVTrim::GPRNoX0RegClass);
  return true;
}

static bool isPhysical(const MachineOperand &MO) {
  return MO.isReg() && MO.getReg().isPhysical();
}

/// Look through \p MI's operands and propagate what it demands to its uses.
void RVTrimVLOptimizerImpl::transfer(const MachineInstr &MI) {
  if (!isSupportedInstr(MI) || !checkUsers(MI) || any_of(MI.defs(), isPhysical))
    DemandedVLs[&MI] = DemandedVL::vlmax();

  for (const MachineOperand &MO : virtual_vec_uses(MI)) {
    const MachineInstr *Def = MRI->getVRegDef(MO.getReg());
    DemandedVL Prev = DemandedVLs[Def];
    DemandedVLs[Def] = DemandedVLs[Def].max(
        *MRI, getMinimumVLForUser(MI, MI.getOperandNo(&MO)));
    if (DemandedVLs[Def] != Prev)
      Worklist.insert(Def);
  }
}

bool RVTrimVLOptimizerImpl::run(MachineFunction &MF) {
  MRI = &MF.getRegInfo();

  const RVTrimSubtarget &ST = MF.getSubtarget<RVTrimSubtarget>();
  if (!ST.hasVInstructions())
    return false;

  TII = ST.getInstrInfo();

  assert(DemandedVLs.empty());

  // For each instruction that defines or uses a vector, propagate the VL it
  // uses to its inputs.
  for (MachineBasicBlock *MBB : post_order(&MF)) {
    for (MachineInstr &MI : reverse(*MBB)) {
      if (MI.isDebugInstr())
        continue;
      if (virtual_vec_defs(MI).empty() && virtual_vec_uses(MI).empty())
        continue;
      Worklist.insert(&MI);
    }
  }

  while (!Worklist.empty()) {
    const MachineInstr *MI = Worklist.front();
    Worklist.remove(MI);
    transfer(*MI);
  }

  // Then go through and see if we can reduce the VL of any instructions to
  // only what's demanded.
  bool MadeChange = false;
  for (auto &[MI, VL] : DemandedVLs) {
    if (!isCandidate(*MI))
      continue;
    if (!tryReduceVL(*const_cast<MachineInstr *>(MI), VL.VL))
      continue;
    MadeChange = true;
  }

  DemandedVLs.clear();
  return MadeChange;
}

bool RVTrimVLOptimizerLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  auto GetMDT = [&]() -> const MachineDominatorTree & {
    return getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  };
  return RVTrimVLOptimizerImpl(GetMDT).run(MF);
}

PreservedAnalyses
RVTrimVLOptimizerPass::run(MachineFunction &MF,
                          MachineFunctionAnalysisManager &MFAM) {
  auto GetMDT = [&]() -> const MachineDominatorTree & {
    return MFAM.getResult<MachineDominatorTreeAnalysis>(MF);
  };
  bool Changed = RVTrimVLOptimizerImpl(GetMDT).run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<MachineRegisterClassAnalysis>();
  return PA;
}
