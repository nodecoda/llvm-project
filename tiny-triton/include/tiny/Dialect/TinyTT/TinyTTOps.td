//===- TinyTTOps.td - tinytt operations --------------------*- tablegen -*-===//
//
// Chapter 1 scope: the twelve TTIR operations, no dot and no layouts.  The
// operation definitions themselves live in `TinyCommonOps.td` because chapter
// 3's `tinyttg` reuses them verbatim; chapter 1 only fixes the dialect.
//
// The verifiers are the real deliverable here: they are what makes `tinytt`
// a dialect rather than "text that happens to parse".
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD_OPS
#define TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD_OPS

include "TinyTTTypes.td"
include "../TinyCommon/TinyCommonOps.td"

def TinyTT_GetProgramIdOp
    : TinyCommon_GetProgramId<TinyTT_Dialect, "get_program_id">;
def TinyTT_MakeRangeOp
    : TinyCommon_MakeRange<TinyTT_Dialect, "make_range">;
def TinyTT_SplatOp
    : TinyCommon_Splat<TinyTT_Dialect, "splat">;
def TinyTT_AddPtrOp
    : TinyCommon_AddPtr<TinyTT_Dialect, "addptr">;
def TinyTT_LoadOp
    : TinyCommon_Load<TinyTT_Dialect, "load">;
def TinyTT_MaskedLoadOp
    : TinyCommon_MaskedLoad<TinyTT_Dialect, "masked_load">;
def TinyTT_StoreOp
    : TinyCommon_Store<TinyTT_Dialect, "store">;
def TinyTT_MaskedStoreOp
    : TinyCommon_MaskedStore<TinyTT_Dialect, "masked_store">;
def TinyTT_ReduceOp
    : TinyCommon_Reduce<TinyTT_Dialect, "reduce">;
def TinyTT_TransOp
    : TinyCommon_Trans<TinyTT_Dialect, "trans">;
def TinyTT_BroadcastOp
    : TinyCommon_Broadcast<TinyTT_Dialect, "broadcast">;
def TinyTT_ExpandDimsOp
    : TinyCommon_ExpandDims<TinyTT_Dialect, "expand_dims">;

#endif // TINY_TRITON_DIALECT_TINYTT_TINYTTTYPES_TD_OPS
