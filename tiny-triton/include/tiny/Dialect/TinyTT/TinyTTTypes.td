//===- TinyTTTypes.td - tinytt types ----------------------*- tablegen -*-===//
//
// The one type that TTIR needs beyond the builtin ones: a pointer that lives
// inside a tensor.  `!tinytt.ptr<f32>` prints the default address space (1);
// shared allocations in TTGIR use `!tinytt.ptr<f32, 2>`.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD
#define TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD

include "TinyTTDialect.td"
include "mlir/IR/AttrTypeBase.td"

class TinyTT_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<TinyTT_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def TinyTT_PtrType : TinyTT_Type<"Ptr", "ptr"> {
  let summary = "pointer into a GPU address space";
  let description = [{
    A pointer value.  Address space 1 is global memory (the default, not
    printed), address space 2 is workgroup/shared memory in `tinyttg`.
  }];

  let parameters = (ins
    "::mlir::Type":$pointee,
    "int64_t":$addressSpace
  );

  let hasCustomAssemblyFormat = 1;

  let extraClassDeclaration = [{
    static constexpr int64_t kGlobalAddressSpace = 1;
    static constexpr int64_t kSharedAddressSpace = 2;
  }];
}

// A ranked tensor of tiny pointers.  TTIR treats a "vector of pointers" as an
// ordinary tensor value, which is what lets `tinytt.addptr` be a tensor op.
def TinyTT_PtrTypeConstraint
    : Type<CPred<"::llvm::isa<::tiny::tt::PtrType>($_self)">,
           "a !tinytt.ptr type", "::tiny::tt::PtrType">;

def TinyTT_PtrTensor : TensorOf<[TinyTT_PtrTypeConstraint]>;

#endif // TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD
