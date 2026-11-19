//===- TinyTTGDialect.td - tinyttg dialect definition -----*- tablegen -*-===//
//
// Tiny Triton, chapter 3.  `tinyttg` is the TTGIR level: the same twelve
// tensor operations as `tinytt`, but every tensor carries a layout, plus the
// five operations that only make sense once threads exist.
//
// `!tinytt.ptr` stays owned by `tinytt`: Triton's TTGIR still uses `tt.ptr`.
// Only tensor types gain an encoding here.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTTG_TINYTTGDIALECT_TD
#define TINY_TRITON_DIALECT_TINYTTG_TINYTTGDIALECT_TD

include "mlir/IR/OpBase.td"

def TinyTTG_Dialect : Dialect {
  let name = "tinyttg";
  let summary = "TTGIR level of the tiny Triton pipeline";
  let description = [{
    `tinyttg` is Triton's TTGIR in miniature.  Its tensor types carry a
    `#tinyttg.blocked` or `#tinyttg.shared` encoding, which is what turns an
    unshaped TTIR value into a value with a thread mapping.
  }];
  let cppNamespace = "::tiny::ttg";
  let useDefaultAttributePrinterParser = 1;
  let dependentDialects = ["::tiny::tt::TinyTTDialect"];
  let extraClassDeclaration = [{
    void registerLayouts();
  }];
}

class TinyTTG_Op<string mnemonic, list<Trait> traits = []>
    : Op<TinyTTG_Dialect, mnemonic, traits>;

#endif // TINY_TRITON_DIALECT_TINYTTG_TINYTTGDIALECT_TD
