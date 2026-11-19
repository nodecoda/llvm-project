//===- TargetInfo.h - the one place that knows GPUMin's thread model ------===//
//
// Requirement I5: conversion patterns must not hard-code 8/4/32.  Every
// number that describes the target lives here.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_CONVERSION_TARGETINFO_H
#define TINY_TRITON_CONVERSION_TARGETINFO_H

#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.h"

#include "llvm/ADT/ArrayRef.h"
#include "mlir/IR/BuiltinTypes.h"

namespace tiny {

/// The tiny pipeline targets exactly one machine: GPUMin, whose blocks are
/// 4 warps of 8 lanes.  Chapter 7 may add a second shape; until then this is
/// a value type, not a registry.
struct TargetInfo {
  int64_t lanesPerWarp = 8;
  int64_t warpsPerCTA = 4;

  int64_t threadsPerBlock() const { return lanesPerWarp * warpsPerCTA; }

  /// The default blocked layout for a rank-1 or rank-2 tensor.  Returns a
  /// null attribute for any other rank, which the pass reports as an error.
  ///
  /// The shape matters: a rank-1 tensor shorter than one warp gets fewer
  /// warps instead of a tile larger than the tensor, and a rank-2 tensor
  /// with a single row or column keeps one warp along that dimension.  This
  /// is the "replicate, do not invent threads" rule of chapter 7.
  ttg::BlockedAttr getDefaultBlocked(mlir::MLIRContext *context,
                                     llvm::ArrayRef<int64_t> shape) const {
    switch (shape.size()) {
    case 1:
      return ttg::BlockedAttr::get(context, {1}, {lanesPerWarp},
                                   {warpsForRank1(shape[0])}, {0});
    case 2:
      // Row 0 is spread over warps, column 1 over lanes.
      return ttg::BlockedAttr::get(context, {1, 1}, {1, lanesPerWarp},
                                   {warpsForRows(shape[0]), 1}, {1, 0});
    default:
      return {};
    }
  }

private:
  /// Warps along a 1-D tensor: one warp per `lanesPerWarp` elements, at most
  /// `warpsPerCTA`, at least one.  A shorter extent means fewer warps take
  /// part; the remaining warps repeat the same coordinates.
  int64_t warpsForRank1(int64_t extent) const {
    int64_t warps = extent <= 0 ? 1 : extent / lanesPerWarp;
    if (warps < 1)
      warps = 1;
    return warps > warpsPerCTA ? warpsPerCTA : warps;
  }

  /// Warps along the row dimension of a 2-D tensor: one warp per row, at most
  /// `warpsPerCTA`.  An 8x8 tile therefore covers four rows per warp step and
  /// repeats twice, which is the coordinate model chapter 7 freezes.
  int64_t warpsForRows(int64_t extent) const {
    int64_t warps = extent <= 0 ? 1 : extent;
    if (warps < 1)
      warps = 1;
    return warps > warpsPerCTA ? warpsPerCTA : warps;
  }
};

} // namespace tiny

#endif // TINY_TRITON_CONVERSION_TARGETINFO_H
