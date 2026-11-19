//===- TinyTTOps.td - tinytt operations --------------------*- tablegen -*-===//
//
// Chapter 1 scope: ten operations, no dot, no vectorization, no layouts.
//
//   get_program_id  make_range  splat  addptr  load  store
//   reduce  trans  broadcast  expand_dims
//
// The verifiers are the real deliverable here: they are what makes `tinytt`
// a dialect rather than "text that happens to parse".
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD_OPS
#define TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD_OPS

include "TinyTTTypes.td"
include "mlir/IR/CommonTypeConstraints.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// Program structure
//===----------------------------------------------------------------------===//

def TinyTT_GetProgramIdOp : TinyTT_Op<"get_program_id", [Pure]> {
  let summary = "block index along one grid axis";
  let arguments = (ins I32Attr:$axis);
  let results = (outs I32:$result);
  let assemblyFormat = "prop-dict attr-dict `:` type($result)";
}

def TinyTT_MakeRangeOp : TinyTT_Op<"make_range", [Pure]> {
  let summary = "0..N-1 offsets of one block axis";
  let arguments = (ins I32Attr:$axis);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = "prop-dict attr-dict `:` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Value construction
//===----------------------------------------------------------------------===//

def TinyTT_SplatOp : TinyTT_Op<"splat", [Pure]> {
  let summary = "broadcast one scalar into every tensor element";
  let arguments = (ins AnyType:$src);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = "$src prop-dict attr-dict `:` type($src) `->` type($result)";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

def TinyTT_AddPtrOp : TinyTT_Op<"addptr",
    [Pure, SameOperandsAndResultShape]> {
  let summary = "elementwise pointer arithmetic";
  let arguments = (ins TinyTT_PtrTensor:$ptr, AnyRankedTensor:$offset);
  let results = (outs TinyTT_PtrTensor:$result);
  let assemblyFormat =
      "$ptr `,` $offset prop-dict attr-dict `:` type($ptr) `,` type($offset) `->` type($result)";
  let hasVerifier = 1;
}

def TinyTT_LoadOp : TinyTT_Op<"load"> {
  let summary = "gather from a pointer tensor";
  let arguments = (ins TinyTT_PtrTensor:$ptr);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $ptr prop-dict attr-dict `:` type($ptr) `,` type($result)
  }];
  let hasVerifier = 1;
}

def TinyTT_MaskedLoadOp : TinyTT_Op<"masked_load"> {
  let summary = "gather with an explicit mask and a fallback value";
  let arguments = (ins
    TinyTT_PtrTensor:$ptr,
    AnyRankedTensor:$mask,
    AnyRankedTensor:$other);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $ptr `,` $mask `,` $other prop-dict attr-dict
      `:` type($ptr) `,` type($mask) `,` type($other) `,` type($result)
  }];
  let hasVerifier = 1;
}

def TinyTT_StoreOp : TinyTT_Op<"store"> {
  let summary = "scatter to a pointer tensor";
  let arguments = (ins TinyTT_PtrTensor:$ptr, AnyRankedTensor:$value);
  let assemblyFormat = [{
    $ptr `,` $value prop-dict attr-dict `:` type($ptr) `,` type($value)
  }];
  let hasVerifier = 1;
}

def TinyTT_MaskedStoreOp : TinyTT_Op<"masked_store"> {
  let summary = "scatter with an explicit mask";
  let arguments = (ins
    TinyTT_PtrTensor:$ptr,
    AnyRankedTensor:$value,
    AnyRankedTensor:$mask);
  let assemblyFormat = [{
    $ptr `,` $value `,` $mask prop-dict attr-dict
      `:` type($ptr) `,` type($value) `,` type($mask)
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Shape-changing
//===----------------------------------------------------------------------===//

def TinyTT_ReduceOp : TinyTT_Op<"reduce", [Pure]> {
  let summary = "block-wide reduction along one axis";
  let description = [{
    Triton-faithful: no `init` operand.  The identity value follows from
    `combine`, which is one of `"add"`, `"max"`, `"min"`; the verifier owns
    that domain.  The reduced dimension becomes 1, it is not dropped.
  }];
  let arguments = (ins AnyRankedTensor:$input, I32Attr:$axis,
                       StrAttr:$combine);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

def TinyTT_TransOp : TinyTT_Op<"trans", [Pure]> {
  let summary = "permute tensor dimensions";
  let arguments = (ins AnyRankedTensor:$input, DenseI32ArrayAttr:$order);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

def TinyTT_BroadcastOp : TinyTT_Op<"broadcast", [Pure]> {
  let summary = "expand size-1 dimensions to a larger shape";
  let arguments = (ins AnyRankedTensor:$input);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

def TinyTT_ExpandDimsOp : TinyTT_Op<"expand_dims", [Pure]> {
  let summary = "insert a size-1 dimension at `axis`";
  let arguments = (ins AnyRankedTensor:$input, I32Attr:$axis);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

#endif // TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD_OPS
