//===--- RVMin.h - Declare RVMin target feature support --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the RVMin TargetInfo object.  RVMin reuses the RISC-V
// ILP32 ABI and object format but only implements RV32I plus the optional
// M, Zbb, Zbs and A extensions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_RVMIN_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_RVMIN_H

#include "RISCV.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY RVMinTargetInfo : public RISCV32TargetInfo {
public:
  RVMinTargetInfo(const llvm::Triple &Triple, const TargetOptions &Opts)
      : RISCV32TargetInfo(Triple, Opts) {
    // The backend has no vector types, no TLS and a fixed ILP32 ABI.
    HasRISCVVTypes = false;
    TLSSupported = false;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {};
  }

  bool setABI(const std::string &Name) override;

  bool isValidCPUName(StringRef Name) const override;
  void fillValidCPUList(SmallVectorImpl<StringRef> &Values) const override;

  bool initFeatureMap(llvm::StringMap<bool> &Features, DiagnosticsEngine &Diags,
                      StringRef CPU,
                      const std::vector<std::string> &FeaturesVec) const override;

  bool handleTargetFeatures(std::vector<std::string> &Features,
                            DiagnosticsEngine &Diags) override;
};

} // namespace targets
} // namespace clang

#endif // LLVM_CLANG_LIB_BASIC_TARGETS_RVMIN_H
