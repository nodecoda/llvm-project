//===- tiny-opt.cpp - tiny Triton optimizer driver -------------*- C++ -*-===//
//
// `tiny-opt` is the dialect/pass driver.  Chapter 1 registers `tiny.tt` plus
// the builtin dialects a TTIR module needs (arith, func, scf) and no passes
// beyond the generic ones mlir-opt ships.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Arith/IR/ArithDialect.h"
#include "mlir/Dialect/Func/IR/FuncDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCFDialect.h"
#include "mlir/InitAllPasses.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "tiny/Conversion/TinyTTGToLLVM.h"
#include "tiny/Conversion/TinyTTToTinyTTG.h"
#include "tiny/Dialect/TinyTT/TinyTTDialect.h"
#include "tiny/Dialect/TinyTTG/TinyTTGDialect.h"

int main(int argc, char **argv) {
  mlir::registerAllPasses();
  tiny::registerTinyTTToTinyTTGPass();
  tiny::registerTinyTTGToLLVMPass();

  mlir::DialectRegistry registry;
  registry.insert<tiny::tt::TinyTTDialect, tiny::ttg::TinyTTGDialect,
                  mlir::arith::ArithDialect,
                  mlir::func::FuncDialect, mlir::scf::SCFDialect,
                  mlir::LLVM::LLVMDialect>();

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "tiny-opt driver\n", registry));
}
