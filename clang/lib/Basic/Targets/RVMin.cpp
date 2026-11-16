//===--- RVMin.cpp - Implement RVMin target feature support ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 25: clang's TargetInfo for RVMin.
//
// The class is deliberately thin: everything about ILP32 data layout, the
// calling convention and the __riscv* macros comes from RISCV32TargetInfo.
// What changes is the feature surface: RVMin only implements RV32I plus
// M/Zbb/Zbs/A, so everything else must either be ignored (+i, +zicsr,
// +zifencei, +relax) or diagnosed (C/F/D/V and the rest).
//
//===----------------------------------------------------------------------===//

#include "RVMin.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/MacroBuilder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

using namespace clang;
using namespace clang::targets;

void RVMinTargetInfo::getTargetDefines(const LangOptions &Opts,
                                       MacroBuilder &Builder) const {
  RISCVTargetInfo::getTargetDefines(Opts, Builder);
  // A tutorial-only marker so a translation unit can tell RVMin apart from
  // riscv32 even though the __riscv* macros are shared.
  Builder.defineMacro("__rvmin");
}

bool RVMinTargetInfo::initFeatureMap(
    llvm::StringMap<bool> &Features, DiagnosticsEngine &Diags, StringRef CPU,
    const std::vector<std::string> &FeaturesVec) const {
  if (!RISCV32TargetInfo::initFeatureMap(Features, Diags, CPU, FeaturesVec))
    return false;
  // RISCV32TargetInfo adds "32bit" so that __has_feature(32bit) works.  That
  // name is also serialized into the module's target-features attribute, and
  // the RVMin backend has no such feature; drop it here instead of teaching
  // TableGen about a feature that changes nothing.
  Features.erase("32bit");
  return true;
}

bool RVMinTargetInfo::setABI(const std::string &Name) {
  if (Name != "ilp32")
    return false;
  ABI = Name;
  return true;
}

static bool isRVMinExtension(StringRef Ext) {
  return Ext == "i" || Ext == "m" || Ext == "a" || Ext == "zbb" ||
         Ext == "zbs" ||
         // Implied by "a" (zalrsc/zaamo) or "m" (zmmul).  The backend has
         // patterns for the operations these unlock, so they are allowed even
         // though they are not spelled out in RVMin.td.
         Ext == "zaamo" || Ext == "zalrsc" || Ext == "zmmul" ||
         Ext == "zicsr" ||
         Ext == "zifencei";
}

bool RVMinTargetInfo::handleTargetFeatures(std::vector<std::string> &Features,
                                           DiagnosticsEngine &Diags) {
  if (!RISCVTargetInfo::handleTargetFeatures(Features, Diags))
    return false;

  for (const auto &Ext : ISAInfo->getExtensions()) {
    if (isRVMinExtension(Ext.first))
      continue;
    Diags.Report(diag::err_invalid_feature_combination)
        << "extension '" + Ext.first + "' is not supported by rvmin";
    return false;
  }

  // The RISC-V driver emits a long "-<ext>" list to switch off every optional
  // extension, plus spellings such as +i/+zicsr/+zifencei/+relax that RVMin
  // does not model.  Keep only the features the backend actually implements;
  // this vector is what ends up in every function's target-features
  // attribute, so leaking the rest would make the backend warn on each run.
  llvm::erase_if(Features, [](const std::string &F) {
    StringRef Name = F;
    if (!Name.consume_front("+"))
      return true;
    return Name != "m" && Name != "a" && Name != "zbb" && Name != "zbs";
  });
  return true;
}
