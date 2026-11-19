//===- TargetInfo.h - the one place that knows GPUMin's thread model ------===//
//
// Requirement I5: conversion patterns must not hard-code 8/4/32.  Every
// number that describes the target lives here.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_CONVERSION_TARGETINFO_H
#define TINY_TRITON_CONVERSION_TARGETINFO_H

#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.h"

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
  ttg::BlockedAttr getDefaultBlocked(mlir::MLIRContext *context,
                                     int64_t rank) const;
};

} // namespace tiny

#endif // TINY_TRITON_CONVERSION_TARGETINFO_H
