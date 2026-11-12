//===-- RVTrimMCTargetDesc.cpp - RISC-V Target Descriptions ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// This file provides RISC-V specific target descriptions.
///
//===----------------------------------------------------------------------===//

#include "RVTrimMCTargetDesc.h"
#include "RVTrimELFStreamer.h"
#include "RVTrimInstPrinter.h"
#include "RVTrimMCAsmInfo.h"
#include "RVTrimMCObjectFileInfo.h"
#include "RVTrimMCOptions.h"
#include "RVTrimTargetStreamer.h"
#include "TargetInfo/RVTrimTargetInfo.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Option/LibraryOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <bitset>

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#define GET_INSTRINFO_NAMED_OPS
#include "RVTrimGenInstrInfo.inc"

#define GET_REGINFO_MC_DESC
#include "RVTrimGenRegisterInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "RVTrimGenSubtargetInfo.inc"

#define OPTIONS_STRUCT_DEFS
#include "RVTrimMCOptions.inc"

using namespace llvm;

static MCInstrInfo *createRVTrimMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitRVTrimMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createRVTrimMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitRVTrimMCRegisterInfo(X, RVTrim::X1);
  return X;
}

static MCAsmInfo *createRVTrimMCAsmInfo(const MCRegisterInfo &MRI,
                                       const Triple &TT,
                                       const MCTargetOptions &Options) {
  MCAsmInfo *MAI = nullptr;
  if (TT.isOSBinFormatELF())
    MAI = new RVTrimMCAsmInfo(TT, Options);
  else if (TT.isOSBinFormatMachO())
    MAI = new RVTrimMCAsmInfoDarwin(Options);
  else
    reportFatalUsageError("unsupported object format");

  unsigned SP = MRI.getDwarfRegNum(RVTrim::X2, true);
  MCCFIInstruction Inst = MCCFIInstruction::cfiDefCfa(nullptr, SP, 0);
  MAI->addInitialFrameState(Inst);

  return MAI;
}

static MCObjectFileInfo *
createRVTrimMCObjectFileInfo(MCContext &Ctx, bool PIC,
                            bool LargeCodeModel = false) {
  MCObjectFileInfo *MOFI = new RVTrimMCObjectFileInfo();
  MOFI->initMCObjectFileInfo(Ctx, PIC, LargeCodeModel);
  return MOFI;
}

void RVTrim::updateCZceFeatureImplications(MCSubtargetInfo &STI) {
  // Add Zcd if C and D are enabled and we aren't targeting 64-bit RVY.
  if (STI.hasFeature(RVTrim::FeatureStdExtC) &&
      STI.hasFeature(RVTrim::FeatureStdExtD) &&
      !STI.hasFeature(RVTrim::FeatureStdExtZcd) &&
      !(STI.hasFeature(RVTrim::Feature64Bit) &&
        STI.hasFeature(RVTrim::FeatureStdExtY)))
    STI.ToggleFeature(RVTrim::FeatureStdExtZcd);

  // Add Zcf if F and C or Zce are enabled on RV32 and Y is not enabled.
  if (!STI.hasFeature(RVTrim::FeatureStdExtZcf) &&
      !STI.hasFeature(RVTrim::Feature64Bit) &&
      !STI.hasFeature(RVTrim::FeatureStdExtY) &&
      STI.hasFeature(RVTrim::FeatureStdExtF) &&
      (STI.hasFeature(RVTrim::FeatureStdExtC) ||
       STI.hasFeature(RVTrim::FeatureStdExtZce)))
    STI.ToggleFeature(RVTrim::FeatureStdExtZcf);

  // We don't infer C or Zce because no code should be looking at those
  // subtarget feature flags and they can cause issues like #229758.
}

static MCSubtargetInfo *
createRVTrimMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  if (CPU.empty() || CPU == "generic")
    CPU = TT.isArch64Bit() ? "generic-rv64" : "generic-rv32";

  MCSubtargetInfo *X =
      createRVTrimMCSubtargetInfoImpl(TT, CPU, /*TuneCPU*/ CPU, FS);

  // If the CPU is "help" fill in 64 or 32 bit feature so we can pass
  // RVTrimFeatures::validate.
  // FIXME: Why does llvm-mc still expect a source file with -mcpu=help?
  if (CPU == "help") {
    llvm::FeatureBitset Features = X->getFeatureBits();
    if (TT.isArch64Bit())
      Features.set(RVTrim::Feature64Bit);
    else
      Features.set(RVTrim::Feature32Bit);
    X->setFeatureBits(Features);
  }

  RVTrim::updateCZceFeatureImplications(*X);

  if (CPU != "help")
    RVTrimFeatures::validateProfile(*X);

  return X;
}

static MCInstPrinter *createRVTrimMCInstPrinter(const Triple &T,
                                               unsigned SyntaxVariant,
                                               const MCAsmInfo &MAI,
                                               const MCInstrInfo &MII,
                                               const MCRegisterInfo &MRI) {
  return new RVTrimInstPrinter(MAI, MII, MRI);
}

static MCTargetStreamer *
createRVTrimObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI) {
  const Triple &TT = STI.getTargetTriple();
  if (TT.isOSBinFormatELF())
    return new RVTrimTargetELFStreamer(S, STI);
  return new RVTrimTargetStreamer(S);
}

static MCStreamer *
createMachOStreamer(MCContext &Ctx, std::unique_ptr<MCAsmBackend> &&TAB,
                    std::unique_ptr<MCObjectWriter> &&OW,
                    std::unique_ptr<MCCodeEmitter> &&Emitter) {
  return createMachOStreamer(Ctx, std::move(TAB), std::move(OW),
                             std::move(Emitter),
                             /*DWARFMustBeAtTheEnd*/ false,
                             /*LabelSections*/ true);
}

static MCTargetStreamer *
createRVTrimAsmTargetStreamer(MCStreamer &S, formatted_raw_ostream &OS,
                             MCInstPrinter *InstPrint) {
  return new RVTrimTargetAsmStreamer(S, OS);
}

static MCTargetStreamer *createRVTrimNullTargetStreamer(MCStreamer &S) {
  return new RVTrimTargetStreamer(S);
}

namespace {

class RVTrimMCInstrAnalysis : public MCInstrAnalysis {
  int64_t GPRState[31] = {};
  std::bitset<31> GPRValidMask;

  static bool isGPR(MCRegister Reg) {
    return Reg >= RVTrim::X0 && Reg <= RVTrim::X31;
  }
  static bool isYGPR(MCRegister Reg) {
    return Reg >= RVTrim::X0_Y && Reg <= RVTrim::X31_Y;
  }
  static bool isZeroReg(MCRegister Reg) {
    return Reg == RVTrim::X0 || Reg == RVTrim::X0_Y;
  }

  static unsigned getRegIndex(MCRegister Reg) {
    if (isYGPR(Reg)) {
      assert(Reg != RVTrim::X0_Y && "Invalid GPR reg");
      return Reg - RVTrim::X1_Y;
    }
    assert(isGPR(Reg) && Reg != RVTrim::X0 && "Invalid GPR reg");
    return Reg - RVTrim::X1;
  }

  void setGPRState(MCRegister Reg, std::optional<int64_t> Value) {
    if (isZeroReg(Reg))
      return;

    auto Index = getRegIndex(Reg);

    if (Value) {
      GPRState[Index] = *Value;
      GPRValidMask.set(Index);
    } else {
      GPRValidMask.reset(Index);
    }
  }

  std::optional<int64_t> getGPRState(MCRegister Reg) const {
    if (isZeroReg(Reg))
      return 0;

    auto Index = getRegIndex(Reg);

    if (GPRValidMask.test(Index))
      return GPRState[Index];
    return std::nullopt;
  }

public:
  explicit RVTrimMCInstrAnalysis(const MCInstrInfo *Info)
      : MCInstrAnalysis(Info) {}

  void resetState() override { GPRValidMask.reset(); }

  void updateState(const MCInst &Inst, const MCSubtargetInfo *STI,
                   uint64_t Addr) override {
    // Terminators mark the end of a basic block which means the sequentially
    // next instruction will be the first of another basic block and the current
    // state will typically not be valid anymore. For calls, we assume all
    // registers may be clobbered by the callee (TODO: should we take the
    // calling convention into account?).
    if (isTerminator(Inst) || isCall(Inst)) {
      resetState();
      return;
    }

    switch (Inst.getOpcode()) {
    default: {
      // Clear the state of all defined registers for instructions that we don't
      // explicitly support.
      auto NumDefs = Info->get(Inst.getOpcode()).getNumDefs();
      for (unsigned I = 0; I < NumDefs; ++I) {
        auto DefReg = Inst.getOperand(I).getReg();
        if (isGPR(DefReg))
          setGPRState(DefReg, std::nullopt);
      }
      break;
    }
    case RVTrim::AUIPC:
      setGPRState(Inst.getOperand(0).getReg(),
                  Addr + SignExtend64<32>(Inst.getOperand(1).getImm() << 12));
      break;
    }
  }

  bool evaluateBranch(const MCInst &Inst, uint64_t Addr, uint64_t Size,
                      uint64_t &Target) const override {
    if (isConditionalBranch(Inst)) {
      int64_t Imm;
      if (Size == 2)
        Imm = Inst.getOperand(1).getImm();
      else
        Imm = Inst.getOperand(2).getImm();
      Target = Addr + Imm;
      return true;
    }

    switch (Inst.getOpcode()) {
    case RVTrim::C_J:
    case RVTrim::C_JAL:
    case RVTrim::QC_E_J:
    case RVTrim::QC_E_JAL:
      Target = Addr + Inst.getOperand(0).getImm();
      return true;
    case RVTrim::JAL:
      Target = Addr + Inst.getOperand(1).getImm();
      return true;
    case RVTrim::JALR: {
      if (auto TargetRegState = getGPRState(Inst.getOperand(1).getReg())) {
        Target = *TargetRegState + Inst.getOperand(2).getImm();
        return true;
      }
      return false;
    }
    }

    return false;
  }

  bool isTerminator(const MCInst &Inst) const override {
    if (MCInstrAnalysis::isTerminator(Inst))
      return true;

    switch (Inst.getOpcode()) {
    default:
      return false;
    case RVTrim::JAL:
    case RVTrim::JALR:
      return Inst.getOperand(0).getReg() == RVTrim::X0;
    }
  }

  bool isCall(const MCInst &Inst) const override {
    if (MCInstrAnalysis::isCall(Inst))
      return true;

    switch (Inst.getOpcode()) {
    default:
      return false;
    case RVTrim::JAL:
    case RVTrim::JALR:
      return Inst.getOperand(0).getReg() != RVTrim::X0;
    }
  }

  bool isReturn(const MCInst &Inst) const override {
    if (MCInstrAnalysis::isReturn(Inst))
      return true;

    switch (Inst.getOpcode()) {
    default:
      return false;
    case RVTrim::JALR:
      return Inst.getOperand(0).getReg() == RVTrim::X0 &&
             maybeReturnAddress(Inst.getOperand(1).getReg());
    case RVTrim::C_JR:
      return maybeReturnAddress(Inst.getOperand(0).getReg());
    }
  }

  bool isBranch(const MCInst &Inst) const override {
    if (MCInstrAnalysis::isBranch(Inst))
      return true;

    return isBranchImpl(Inst);
  }

  bool isUnconditionalBranch(const MCInst &Inst) const override {
    if (MCInstrAnalysis::isUnconditionalBranch(Inst))
      return true;

    return isBranchImpl(Inst);
  }

  bool isIndirectBranch(const MCInst &Inst) const override {
    if (MCInstrAnalysis::isIndirectBranch(Inst))
      return true;

    switch (Inst.getOpcode()) {
    default:
      return false;
    case RVTrim::JALR:
      return Inst.getOperand(0).getReg() == RVTrim::X0 &&
             !maybeReturnAddress(Inst.getOperand(1).getReg());
    case RVTrim::C_JR:
      return !maybeReturnAddress(Inst.getOperand(0).getReg());
    }
  }

  /// Returns (PLT virtual address, GOT virtual address) pairs for PLT entries.
  std::vector<std::pair<uint64_t, uint64_t>>
  findPltEntries(uint64_t PltSectionVA, ArrayRef<uint8_t> PltContents,
                 const MCSubtargetInfo &STI) const override {
    if (!STI.getTargetTriple().isRVTrim64())
      return {};
    const uint32_t LoadInsnOpCode = 0x3003; // ld

    constexpr uint64_t FirstEntryAt = 32, EntrySize = 16;
    if (PltContents.size() < FirstEntryAt + EntrySize)
      return {};

    std::vector<std::pair<uint64_t, uint64_t>> Results;
    for (uint64_t EntryStart = FirstEntryAt,
                  EntryStartEnd = PltContents.size() - EntrySize;
         EntryStart <= EntryStartEnd; EntryStart += EntrySize) {
      const uint32_t AuipcInsn =
          support::endian::read32le(PltContents.data() + EntryStart);
      const bool IsAuipc = (AuipcInsn & 0x7F) == 0x17;
      if (!IsAuipc)
        continue;

      const uint32_t LoadInsn =
          support::endian::read32le(PltContents.data() + EntryStart + 4);
      const bool IsLoad = (LoadInsn & 0x707F) == LoadInsnOpCode;
      if (!IsLoad)
        continue;

      const uint64_t GotPltSlotVA = PltSectionVA + EntryStart +
                                    (AuipcInsn & 0xFFFFF000) +
                                    SignExtend64<12>(LoadInsn >> 20);
      Results.emplace_back(PltSectionVA + EntryStart, GotPltSlotVA);
    }

    return Results;
  }

private:
  static bool maybeReturnAddress(MCRegister Reg) {
    // X1 is used for normal returns, X5 for returns from outlined functions.
    return Reg == RVTrim::X1 || Reg == RVTrim::X5;
  }

  static bool isBranchImpl(const MCInst &Inst) {
    switch (Inst.getOpcode()) {
    default:
      return false;
    case RVTrim::JAL:
      return Inst.getOperand(0).getReg() == RVTrim::X0;
    case RVTrim::JALR:
      return Inst.getOperand(0).getReg() == RVTrim::X0 &&
             !maybeReturnAddress(Inst.getOperand(1).getReg());
    case RVTrim::C_JR:
      return !maybeReturnAddress(Inst.getOperand(0).getReg());
    }
  }
};

} // end anonymous namespace

static MCInstrAnalysis *createRVTrimInstrAnalysis(const MCInstrInfo *Info) {
  return new RVTrimMCInstrAnalysis(Info);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeRVTrimTargetMC() {
  static opt::RegisterLibraryOptions<RVTrimMCOptions> O;
  for (Target *T : {&getTheRVTrimTarget()}) {
    TargetRegistry::RegisterMCAsmInfo(*T, createRVTrimMCAsmInfo);
    TargetRegistry::RegisterMCObjectFileInfo(*T, createRVTrimMCObjectFileInfo);
    TargetRegistry::RegisterMCInstrInfo(*T, createRVTrimMCInstrInfo);
    TargetRegistry::RegisterMCRegInfo(*T, createRVTrimMCRegisterInfo);
    TargetRegistry::RegisterMCAsmBackend(*T, createRVTrimAsmBackend);
    TargetRegistry::RegisterMCCodeEmitter(*T, createRVTrimMCCodeEmitter);
    TargetRegistry::RegisterMCInstPrinter(*T, createRVTrimMCInstPrinter);
    TargetRegistry::RegisterMCSubtargetInfo(*T, createRVTrimMCSubtargetInfo);
    TargetRegistry::RegisterELFStreamer(*T, createRVTrimELFStreamer);
    TargetRegistry::RegisterMachOStreamer(*T, createMachOStreamer);
    TargetRegistry::RegisterObjectTargetStreamer(
        *T, createRVTrimObjectTargetStreamer);
    TargetRegistry::RegisterMCInstrAnalysis(*T, createRVTrimInstrAnalysis);

    // Register the asm target streamer.
    TargetRegistry::RegisterAsmTargetStreamer(*T, createRVTrimAsmTargetStreamer);
    // Register the null target streamer.
    TargetRegistry::RegisterNullTargetStreamer(*T,
                                               createRVTrimNullTargetStreamer);
  }
}
