//===-- RVTrimTargetStreamer.cpp - RISC-V Target Streamer Methods ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides RISC-V specific target streamer methods.
//
//===----------------------------------------------------------------------===//

#include "RVTrimTargetStreamer.h"
#include "RVTrimBaseInfo.h"
#include "RVTrimMCTargetDesc.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/RISCVAttributes.h"
#include "llvm/TargetParser/RISCVISAInfo.h"

using namespace llvm;

RVTrimTargetStreamer::RVTrimTargetStreamer(MCStreamer &S)
    : MCTargetStreamer(S),
      CLOpts(RVTrim_MC::getCLOpts(S.getContext().getTargetOptions())) {}

void RVTrimTargetStreamer::finish() { finishAttributeSection(); }
void RVTrimTargetStreamer::reset() {}

void RVTrimTargetStreamer::emitDirectiveOptionArch(
    ArrayRef<RVTrimOptionArchArg> Args) {}
void RVTrimTargetStreamer::emitDirectiveOptionExact() {}
void RVTrimTargetStreamer::emitDirectiveOptionNoExact() {}
void RVTrimTargetStreamer::emitDirectiveOptionPIC() {}
void RVTrimTargetStreamer::emitDirectiveOptionNoPIC() {}
void RVTrimTargetStreamer::emitDirectiveOptionPop() {}
void RVTrimTargetStreamer::emitDirectiveOptionPush() {}
void RVTrimTargetStreamer::emitDirectiveOptionRelax() {}
void RVTrimTargetStreamer::emitDirectiveOptionNoRelax() {}
void RVTrimTargetStreamer::emitDirectiveOptionRVC() {}
void RVTrimTargetStreamer::emitDirectiveOptionNoRVC() {}
void RVTrimTargetStreamer::emitDirectiveVariantCC(MCSymbol &Symbol) {}
void RVTrimTargetStreamer::emitAttribute(unsigned Attribute, unsigned Value) {}
void RVTrimTargetStreamer::finishAttributeSection() {}
void RVTrimTargetStreamer::emitTextAttribute(unsigned Attribute,
                                            StringRef String) {}
void RVTrimTargetStreamer::emitIntTextAttribute(unsigned Attribute,
                                               unsigned IntValue,
                                               StringRef StringValue) {}

void RVTrimTargetStreamer::setTargetABI(RVTrimABI::ABI ABI) {
  assert(ABI != RVTrimABI::ABI_Unknown && "Improperly initialized target ABI");
  TargetABI = ABI;
}

void RVTrimTargetStreamer::setFlagsFromFeatures(const MCSubtargetInfo &STI) {
  HasRVC = STI.hasFeature(RVTrim::FeatureStdExtZca);
  HasTSO = STI.hasFeature(RVTrim::FeatureStdExtZtso);
}

void RVTrimTargetStreamer::emitTargetAttributes(const MCSubtargetInfo &STI,
                                               bool EmitStackAlign) {
  if (EmitStackAlign) {
    unsigned StackAlign;
    if (TargetABI == RVTrimABI::ABI_ILP32E)
      StackAlign = 4;
    else if (TargetABI == RVTrimABI::ABI_LP64E)
      StackAlign = 8;
    else
      StackAlign = 16;
    emitAttribute(RISCVAttrs::STACK_ALIGN, StackAlign);
  }

  auto ParseResult = RVTrimFeatures::parseFeatureBits(STI);
  if (!ParseResult) {
    report_fatal_error(ParseResult.takeError());
  } else {
    auto &ISAInfo = *ParseResult;
    std::string Arch = ISAInfo->toString();
    emitTextAttribute(RISCVAttrs::ARCH, Arch);
    setArchString(Arch);
  }

  if (CLOpts.abi_attributes && STI.hasFeature(RVTrim::FeatureStdExtA)) {
    unsigned AtomicABITag;
    if (STI.hasFeature(RVTrim::FeatureStdExtZalasr))
      AtomicABITag = static_cast<unsigned>(RISCVAttrs::RISCVAtomicAbiTag::A7);
    else if (STI.hasFeature(RVTrim::FeatureNoTrailingSeqCstFence))
      AtomicABITag = static_cast<unsigned>(RISCVAttrs::RISCVAtomicAbiTag::A6C);
    else
      AtomicABITag = static_cast<unsigned>(RISCVAttrs::RISCVAtomicAbiTag::A6S);
    emitAttribute(RISCVAttrs::ATOMIC_ABI, AtomicABITag);
  }
}

// This part is for ascii assembly output
RVTrimTargetAsmStreamer::RVTrimTargetAsmStreamer(MCStreamer &S,
                                               formatted_raw_ostream &OS)
    : RVTrimTargetStreamer(S), OS(OS) {}

void RVTrimTargetAsmStreamer::emitDirectiveOptionPush() {
  OS << "\t.option\tpush\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionPop() {
  OS << "\t.option\tpop\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionPIC() {
  OS << "\t.option\tpic\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionNoPIC() {
  OS << "\t.option\tnopic\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionRVC() {
  OS << "\t.option\trvc\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionNoRVC() {
  OS << "\t.option\tnorvc\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionExact() {
  OS << "\t.option\texact\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionNoExact() {
  OS << "\t.option\tnoexact\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionRelax() {
  OS << "\t.option\trelax\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionNoRelax() {
  OS << "\t.option\tnorelax\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveOptionArch(
    ArrayRef<RVTrimOptionArchArg> Args) {
  OS << "\t.option\tarch";
  for (const auto &Arg : Args) {
    OS << ", ";
    switch (Arg.Type) {
    case RVTrimOptionArchArgType::Full:
      break;
    case RVTrimOptionArchArgType::Plus:
      OS << "+";
      break;
    case RVTrimOptionArchArgType::Minus:
      OS << "-";
      break;
    }
    OS << Arg.Value;
  }
  OS << "\n";
}

void RVTrimTargetAsmStreamer::emitDirectiveVariantCC(MCSymbol &Symbol) {
  OS << "\t.variant_cc\t" << Symbol.getName() << "\n";
}

void RVTrimTargetAsmStreamer::emitAttribute(unsigned Attribute, unsigned Value) {
  OS << "\t.attribute\t" << Attribute << ", " << Twine(Value) << "\n";
}

void RVTrimTargetAsmStreamer::emitTextAttribute(unsigned Attribute,
                                               StringRef String) {
  OS << "\t.attribute\t" << Attribute << ", \"" << String << "\"\n";
}

void RVTrimTargetAsmStreamer::emitIntTextAttribute(unsigned Attribute,
                                                  unsigned IntValue,
                                                  StringRef StringValue) {}

void RVTrimTargetAsmStreamer::finishAttributeSection() {}
