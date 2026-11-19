//===- TinyTTDialect.td - tinytt dialect definition -------*- tablegen -*-===//
//
// Tiny Triton, chapter 1.  `tinytt` is the TTIR level: Triton-style tensor
// programs with no thread mapping.  Layouts appear only in `tinyttg`.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTT_TINYTTDIALECT_TD
#define TINY_TRITON_DIALECT_TINYTT_TINYTTDIALECT_TD

include "mlir/IR/OpBase.td"

def TinyTT_Dialect : Dialect {
  let name = "tinytt";
  let summary = "TTIR level of the tiny Triton pipeline";
  let description = [{
    `tinytt` mirrors the Triton TTIR layer: tensor values, pointer tensors and
    block-level program ids.  Values are unshaped by any thread mapping; the
    `tiny-convert-tt-to-ttg` pass adds layouts and produces `tinyttg`.
  }];
  let cppNamespace = "::tiny::tt";
  let useDefaultTypePrinterParser = 1;
  let extraClassDeclaration = [{
    void registerTypes();
  }];
}

class TinyTT_Op<string mnemonic, list<Trait> traits = []>
    : Op<TinyTT_Dialect, mnemonic, traits>;

#endif // TINY_TRITON_DIALECT_TINYTT_TINYTTDIALECT_TD
