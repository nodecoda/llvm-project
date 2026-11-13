//===-- MiniRVMCInstLower.h - Lower MachineInstr to MCInst ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVMCINSTLOWER_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVMCINSTLOWER_H

#include "llvm/Support/Compiler.h"

namespace llvm {

class AsmPrinter;
class MCContext;
class MCInst;
class MachineInstr;

class LLVM_LIBRARY_VISIBILITY MiniRVMCInstLower {
  MCContext &Ctx;
  AsmPrinter &Printer;

public:
  MiniRVMCInstLower(MCContext &Ctx, AsmPrinter &Printer)
      : Ctx(Ctx), Printer(Printer) {}

  void Lower(const MachineInstr *MI, MCInst &OutMI) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVMCINSTLOWER_H
