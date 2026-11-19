//===-- EmitTTIR.h - AST -> tinytt lowering ---------------------*- C++ -*-===//
//
// Chapter 2: the frontend lowers the CPython AST straight into `tinytt` ops.
// There is no private IR in between -- the MLIR module *is* the output, and
// `tiny-opt` can read it back.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_TOOLS_FRONTEND_EMITTTIR_H
#define TINY_TRITON_TOOLS_FRONTEND_EMITTTIR_H

#include "PyAst.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include <string>
#include <vector>

namespace tinytriton {

struct KernelArg {
  std::string Name;
  unsigned Line = 0;
};

struct KernelDecl {
  std::string Name;
  unsigned Line = 0;
  std::vector<KernelArg> Args;
};

/// Kernels are `def`s carrying a `@triton.jit` / `@jit` decorator.
llvm::Expected<std::vector<KernelDecl>> parseKernels(const PyAst &Ast);

struct FrontendOptions {
  /// Empty selects the first kernel in the file.
  std::string Kernel;
  /// Argument name -> MLIR type.  Pointer arguments must be bound explicitly
  /// (`--arg-type a_ptr=ptr`); everything else has no default either.
  llvm::StringMap<mlir::Type> ArgTypes;
};

llvm::Expected<mlir::OwningOpRef<mlir::ModuleOp>>
emitTTIR(mlir::MLIRContext &Ctx, const PyAst &Ast, const FrontendOptions &Opts);

} // namespace tinytriton

#endif // TINY_TRITON_TOOLS_FRONTEND_EMITTTIR_H
