//===-- RVTrimTargetStreamer.h - RISC-V Target Streamer ---------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimTARGETSTREAMER_H
#define LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimTARGETSTREAMER_H

#include "RVTrim.h"
#include "RVTrimMCOptions.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"

namespace llvm {

class formatted_raw_ostream;

enum class RVTrimOptionArchArgType {
  Full,
  Plus,
  Minus,
};

struct RVTrimOptionArchArg {
  RVTrimOptionArchArgType Type;
  std::string Value;

  RVTrimOptionArchArg(RVTrimOptionArchArgType Type, std::string Value)
      : Type(Type), Value(Value) {}
};

class RVTrimTargetStreamer : public MCTargetStreamer {
  const RVTrimMCOptions &CLOpts;
  RVTrimABI::ABI TargetABI = RVTrimABI::ABI_Unknown;
  bool HasRVC = false;
  bool HasTSO = false;

public:
  RVTrimTargetStreamer(MCStreamer &S);
  void finish() override;
  virtual void reset();

  virtual void emitDirectiveOptionArch(ArrayRef<RVTrimOptionArchArg> Args);
  virtual void emitDirectiveOptionExact();
  virtual void emitDirectiveOptionNoExact();
  virtual void emitDirectiveOptionPIC();
  virtual void emitDirectiveOptionNoPIC();
  virtual void emitDirectiveOptionPop();
  virtual void emitDirectiveOptionPush();
  virtual void emitDirectiveOptionRelax();
  virtual void emitDirectiveOptionNoRelax();
  virtual void emitDirectiveOptionRVC();
  virtual void emitDirectiveOptionNoRVC();
  virtual void setArchString(StringRef Arch) {}
  virtual void emitDirectiveVariantCC(MCSymbol &Symbol);
  virtual void emitAttribute(unsigned Attribute, unsigned Value);
  virtual void finishAttributeSection();
  virtual void emitTextAttribute(unsigned Attribute, StringRef String);
  virtual void emitIntTextAttribute(unsigned Attribute, unsigned IntValue,
                                    StringRef StringValue);

  void emitTargetAttributes(const MCSubtargetInfo &STI, bool EmitStackAlign);
  void setTargetABI(RVTrimABI::ABI ABI);
  RVTrimABI::ABI getTargetABI() const { return TargetABI; }
  bool hasTargetABI() const { return TargetABI != RVTrimABI::ABI_Unknown; }
  virtual void setFlagsFromFeatures(const MCSubtargetInfo &STI);
  bool hasRVC() const { return HasRVC; }
  bool hasTSO() const { return HasTSO; }
};

// This part is for ascii assembly output
class RVTrimTargetAsmStreamer : public RVTrimTargetStreamer {
  formatted_raw_ostream &OS;

  void finishAttributeSection() override;
  void emitAttribute(unsigned Attribute, unsigned Value) override;
  void emitTextAttribute(unsigned Attribute, StringRef String) override;
  void emitIntTextAttribute(unsigned Attribute, unsigned IntValue,
                            StringRef StringValue) override;

public:
  RVTrimTargetAsmStreamer(MCStreamer &S, formatted_raw_ostream &OS);

  void emitDirectiveOptionArch(ArrayRef<RVTrimOptionArchArg> Args) override;
  void emitDirectiveOptionExact() override;
  void emitDirectiveOptionNoExact() override;
  void emitDirectiveOptionPIC() override;
  void emitDirectiveOptionNoPIC() override;
  void emitDirectiveOptionPop() override;
  void emitDirectiveOptionPush() override;
  void emitDirectiveOptionRelax() override;
  void emitDirectiveOptionNoRelax() override;
  void emitDirectiveOptionRVC() override;
  void emitDirectiveOptionNoRVC() override;
  void emitDirectiveVariantCC(MCSymbol &Symbol) override;
};

}
#endif
