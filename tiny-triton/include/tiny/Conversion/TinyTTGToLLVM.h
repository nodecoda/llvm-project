//===- TinyTTGToLLVM.h - TTGIR to the LLVM dialect ---------------*- C++ -*-===//

#ifndef TINY_TRITON_CONVERSION_TINYTTGTOLLVM_H
#define TINY_TRITON_CONVERSION_TINYTTGTOLLVM_H

#include <memory>

namespace mlir {
class Pass;
} // namespace mlir

namespace tiny {

std::unique_ptr<mlir::Pass> createTinyTTGToLLVMPass();
void registerTinyTTGToLLVMPass();

} // namespace tiny

#endif // TINY_TRITON_CONVERSION_TINYTTGTOLLVM_H
