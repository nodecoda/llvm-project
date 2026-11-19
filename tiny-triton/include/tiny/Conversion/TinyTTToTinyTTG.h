//===- TinyTTToTinyTTG.h - assign thread mappings -----------------*- C++ -*-===//

#ifndef TINY_TRITON_CONVERSION_TINYTTTOTINYTTG_H
#define TINY_TRITON_CONVERSION_TINYTTTOTINYTTG_H

#include <memory>

namespace mlir {
class Pass;
} // namespace mlir

namespace tiny {

std::unique_ptr<mlir::Pass> createTinyTTToTinyTTGPass();

/// Registers `--tiny-convert-tt-to-ttg` with the global pass registry.
void registerTinyTTToTinyTTGPass();

} // namespace tiny

#endif // TINY_TRITON_CONVERSION_TINYTTTOTINYTTG_H
