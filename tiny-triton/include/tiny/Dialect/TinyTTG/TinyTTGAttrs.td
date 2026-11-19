//===- TinyTTGAttrs.td - tinyttg layouts ------------------*- tablegen -*-===//
//
// The two layouts of the tiny pipeline.  `blocked` says how a tile is spread
// over the lanes of a block; `shared` says how a shared-memory buffer is laid
// out.  Larger tensors repeat the blocked tile, so coverage is a divisibility
// rule, not an equality rule.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_DIALECT_TINYTTG_TINYTTGATTRS_TD
#define TINY_TRITON_DIALECT_TINYTTG_TINYTTGATTRS_TD

include "TinyTTGDialect.td"
include "mlir/IR/AttrTypeBase.td"

class TinyTTG_Attr<string name, string mnem> : AttrDef<TinyTTG_Dialect, name> {
  let mnemonic = mnem;
}

def TinyTTG_BlockedAttr : TinyTTG_Attr<"Blocked", "blocked"> {
  let summary = "tile distributed over threadsPerWarp x warpsPerCTA lanes";
  let description = [{
    One tile holds `sizePerThread[d] * threadsPerWarp[d] * warpsPerCTA[d]`
    elements along dimension `d`.  A tensor larger than one tile repeats the
    tile `order`-major, which is why the verifier demands divisibility rather
    than equality.
  }];

  let parameters = (ins
    ArrayRefParameter<"int64_t">:$sizePerThread,
    ArrayRefParameter<"int64_t">:$threadsPerWarp,
    ArrayRefParameter<"int64_t">:$warpsPerCTA,
    ArrayRefParameter<"int64_t">:$order
  );

  let hasCustomAssemblyFormat = 1;
  let genVerifyDecl = 1;

  let extraClassDeclaration = [{
    int64_t getRank() const { return getSizePerThread().size(); }

    /// Elements covered by one tile along `dim`.
    int64_t getTileSize(int64_t dim) const {
      return getSizePerThread()[dim] * getThreadsPerWarp()[dim] *
             getWarpsPerCTA()[dim];
    }
  }];
}

def TinyTTG_SharedAttr : TinyTTG_Attr<"Shared", "shared"> {
  let summary = "row-major shared-memory buffer layout";

  let parameters = (ins
    "int64_t":$offset,
    ArrayRefParameter<"int64_t">:$order
  );

  let hasCustomAssemblyFormat = 1;
  let genVerifyDecl = 1;
}

#endif // TINY_TRITON_DIALECT_TINYTTG_TINYTTGATTRS_TD
