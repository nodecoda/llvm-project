//===- TinyTTGOps.h - tinyttg operations -----------------------*- C++ -*-===//

#ifndef TINY_TRITON_DIALECT_TINYTTG_TINYTTGOPS_H
#define TINY_TRITON_DIALECT_TINYTTG_TINYTTGOPS_H

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "tiny/Dialect/TinyTT/TinyTTTypes.h"
#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.h"
#include "tiny/Dialect/TinyTTG/TinyTTGDialect.h"

// `NativeOpTrait<"tiny::RequireLayouts">` is emitted as
// `::mlir::OpTrait::tiny::RequireLayouts`, so the trait lives where MLIR
// resolves native traits.
namespace mlir::OpTrait::tiny {

/// Defined in TinyTTGOps.cpp: every tensor operand/result must carry a
/// layout, and a blocked layout must tile the shape exactly.
mlir::LogicalResult verifyLayouts(mlir::Operation *op);

template <typename ConcreteType>
class RequireLayouts : public TraitBase<ConcreteType, RequireLayouts> {
public:
  static mlir::LogicalResult verifyTrait(mlir::Operation *op) {
    return verifyLayouts(op);
  }
};

} // namespace mlir::OpTrait::tiny

#define GET_OP_CLASSES
#include "tiny/Dialect/TinyTTG/TinyTTGOps.h.inc"

#endif // TINY_TRITON_DIALECT_TINYTTG_TINYTTGOPS_H
