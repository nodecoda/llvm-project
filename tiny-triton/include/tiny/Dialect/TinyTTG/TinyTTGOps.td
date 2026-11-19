//===- TinyTTGOps.td - tinyttg operations -----------------*- tablegen -*-===//
//
// The twelve TTIR operations are reused verbatim through `TinyCommonOps.td`;
// the only difference is the extra `RequireLayouts` trait, which is where the
// TTGIR verifier lives.  Five operations are new: they describe the thread
// mapping itself (convert_layout, shared staging, barrier).
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTTG_TINYTTGOPS_TD
#define TINY_TRITON_DIALECT_TINYTTG_TINYTTGOPS_TD

include "TinyTTGDialect.td"
include "TinyTTGAttrs.td"
include "../TinyCommon/TinyCommonOps.td"

// Every tensor operand/result must carry a `#tinyttg` encoding, and blocked
// layouts must tile the tensor shape exactly (divisibility, not equality).
def TinyTTG_Layouts : NativeOpTrait<"tiny::RequireLayouts">;

//===----------------------------------------------------------------------===//
// The twelve TTIR operations, now with layouts
//===----------------------------------------------------------------------===//

def TinyTTG_GetProgramIdOp
    : TinyCommon_GetProgramId<TinyTTG_Dialect, "get_program_id", [TinyTTG_Layouts]>;
def TinyTTG_MakeRangeOp
    : TinyCommon_MakeRange<TinyTTG_Dialect, "make_range", [TinyTTG_Layouts]>;
def TinyTTG_SplatOp
    : TinyCommon_Splat<TinyTTG_Dialect, "splat", [TinyTTG_Layouts]>;
def TinyTTG_AddPtrOp
    : TinyCommon_AddPtr<TinyTTG_Dialect, "addptr", [TinyTTG_Layouts]>;
def TinyTTG_LoadOp
    : TinyCommon_Load<TinyTTG_Dialect, "load", [TinyTTG_Layouts]>;
def TinyTTG_MaskedLoadOp
    : TinyCommon_MaskedLoad<TinyTTG_Dialect, "masked_load", [TinyTTG_Layouts]>;
def TinyTTG_StoreOp
    : TinyCommon_Store<TinyTTG_Dialect, "store", [TinyTTG_Layouts]>;
def TinyTTG_MaskedStoreOp
    : TinyCommon_MaskedStore<TinyTTG_Dialect, "masked_store", [TinyTTG_Layouts]>;
def TinyTTG_ReduceOp
    : TinyCommon_Reduce<TinyTTG_Dialect, "reduce", [TinyTTG_Layouts]>;
def TinyTTG_StoreScalarOp
    : TinyCommon_StoreScalar<TinyTTG_Dialect, "store_scalar", [TinyTTG_Layouts]>;
def TinyTTG_TransOp
    : TinyCommon_Trans<TinyTTG_Dialect, "trans", [TinyTTG_Layouts]>;
def TinyTTG_BroadcastOp
    : TinyCommon_Broadcast<TinyTTG_Dialect, "broadcast", [TinyTTG_Layouts]>;
def TinyTTG_ExpandDimsOp
    : TinyCommon_ExpandDims<TinyTTG_Dialect, "expand_dims", [TinyTTG_Layouts]>;

//===----------------------------------------------------------------------===//
// Thread mapping
//===----------------------------------------------------------------------===//

def TinyTTG_ConvertLayoutOp
    : TinyTTG_Op<"convert_layout", [Pure, TinyTTG_Layouts]> {
  let summary = "re-distribute one tensor over another layout";
  let arguments = (ins AnyRankedTensor:$src);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = "$src prop-dict attr-dict `:` type($src) `->` type($result)";
  let hasVerifier = 1;
}

def TinyTTG_LocalAllocOp : TinyTTG_Op<"local_alloc", [TinyTTG_Layouts]> {
  let summary = "materialise a tensor in shared memory";
  let arguments = (ins AnyRankedTensor:$src);
  let results = (outs TinyCommon_PtrTypeConstraint:$result);
  let assemblyFormat = "$src attr-dict `:` type($src) `->` type($result)";
  let hasVerifier = 1;
}

def TinyTTG_LocalStoreOp : TinyTTG_Op<"local_store", [TinyTTG_Layouts]> {
  let summary = "write a tensor through a shared-memory pointer";
  let arguments = (ins AnyRankedTensor:$value, TinyCommon_PtrTypeConstraint:$ptr);
  let assemblyFormat = "$value `,` $ptr attr-dict `:` type($value) `,` type($ptr)";
  let hasVerifier = 1;
}

def TinyTTG_LocalLoadOp : TinyTTG_Op<"local_load", [TinyTTG_Layouts]> {
  let summary = "read a tensor back from shared memory";
  let arguments = (ins TinyCommon_PtrTypeConstraint:$ptr);
  let results = (outs AnyRankedTensor:$result);
  let assemblyFormat = "$ptr attr-dict `:` type($ptr) `->` type($result)";
  let hasVerifier = 1;
}

def TinyTTG_BarrierOp : TinyTTG_Op<"barrier"> {
  let summary = "block-wide execution barrier";
  let assemblyFormat = "attr-dict";
}

#endif // TINY_TRITON_DIALECT_TINYTTG_TINYTTGOPS_TD
