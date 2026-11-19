//===- TinyCommonOps.td - ops shared by tinytt and tinyttg --*- tablegen -*-===//
//
// Both dialect levels describe the same twelve operations; they differ in the
// dialect name and in what the verifiers demand (tinyttg requires a layout on
// every tensor).  Defining the ops once, parameterised by dialect, keeps the
// two levels from drifting apart.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYCOMMON_TINYCOMMONOPS_TD
#define TINY_TRITON_DIALECT_TINYCOMMON_TINYCOMMONOPS_TD

include "mlir/IR/OpBase.td"
include "mlir/IR/CommonTypeConstraints.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

// The pointer type itself lives in `tinytt`; both levels share it, so the
// constraint lives here rather than in a dialect-specific file.
def TinyCommon_PtrTypeConstraint
    : Type<CPred<"::llvm::isa<::tiny::tt::PtrType>($_self)">,
           "a tiny pointer type", "::tiny::tt::PtrType">;

def TinyCommon_PtrTensor : TensorOf<[TinyCommon_PtrTypeConstraint]>;

class TinyCommon_GetProgramId<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "block index along one grid axis";
  let arguments = (ins I32Attr:$axis);
  let results = (outs I32:$result);
  let assemblyFormat = "prop-dict attr-dict `:` type($result)";
}

class TinyCommon_MakeRange<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "0..N-1 offsets of one block axis";
  let arguments = (ins I32Attr:$axis);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = "prop-dict attr-dict `:` type($result)";
  let hasVerifier = 1;
}

class TinyCommon_Splat<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "broadcast one scalar into every tensor element";
  let arguments = (ins AnyType:$src);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = "$src prop-dict attr-dict `:` type($src) `->` type($result)";
  let hasVerifier = 1;
}

class TinyCommon_AddPtr<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure, SameOperandsAndResultShape], traits)> {
  let summary = "elementwise pointer arithmetic";
  let arguments = (ins TinyCommon_PtrTensor:$ptr, AnyRankedTensor:$offset);
  let results = (outs TinyCommon_PtrTensor:$result);
  let assemblyFormat = [{
    $ptr `,` $offset prop-dict attr-dict
      `:` type($ptr) `,` type($offset) `->` type($result)
  }];
  let hasVerifier = 1;
}

class TinyCommon_Load<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, traits> {
  let summary = "gather from a pointer tensor";
  let arguments = (ins TinyCommon_PtrTensor:$ptr);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $ptr prop-dict attr-dict `:` type($ptr) `,` type($result)
  }];
  let hasVerifier = 1;
}

class TinyCommon_MaskedLoad<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, traits> {
  let summary = "gather with an explicit mask and a fallback value";
  let arguments = (ins
    TinyCommon_PtrTensor:$ptr,
    AnyRankedTensor:$mask,
    AnyRankedTensor:$other);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $ptr `,` $mask `,` $other prop-dict attr-dict
      `:` type($ptr) `,` type($mask) `,` type($other) `,` type($result)
  }];
  let hasVerifier = 1;
}

class TinyCommon_Store<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, traits> {
  let summary = "scatter to a pointer tensor";
  let arguments = (ins TinyCommon_PtrTensor:$ptr, AnyRankedTensor:$value);
  let assemblyFormat = [{
    $ptr `,` $value prop-dict attr-dict `:` type($ptr) `,` type($value)
  }];
  let hasVerifier = 1;
}

class TinyCommon_MaskedStore<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, traits> {
  let summary = "scatter with an explicit mask";
  let arguments = (ins
    TinyCommon_PtrTensor:$ptr,
    AnyRankedTensor:$value,
    AnyRankedTensor:$mask);
  let assemblyFormat = [{
    $ptr `,` $value `,` $mask prop-dict attr-dict
      `:` type($ptr) `,` type($value) `,` type($mask)
  }];
  let hasVerifier = 1;
}

class TinyCommon_Reduce<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "block-wide reduction along one axis";
  let description = [{
    Triton-faithful: no `init` operand.  The identity value follows from
    `combine`, which is one of `"add"`, `"max"`, `"min"`.
  }];
  let arguments = (ins AnyRankedTensor:$input, I32Attr:$axis,
                       StrAttr:$combine);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

class TinyCommon_Trans<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "permute tensor dimensions";
  let arguments = (ins AnyRankedTensor:$input, DenseI32ArrayAttr:$order);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

class TinyCommon_Broadcast<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "expand size-1 dimensions to a larger shape";
  let arguments = (ins AnyRankedTensor:$input);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

class TinyCommon_ExpandDims<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> {
  let summary = "insert a size-1 dimension at `axis`";
  let arguments = (ins AnyRankedTensor:$input, I32Attr:$axis);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = [{
    $input prop-dict attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
}

#endif // TINY_TRITON_DIALECT_TINYCOMMON_TINYCOMMONOPS_TD
