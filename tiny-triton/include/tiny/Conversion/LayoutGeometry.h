//===- LayoutGeometry.h - what a blocked layout actually covers -----------===//
//
// Chapter 7 adds ranks above one, so the arithmetic that used to be written
// twice (once in the TT->TTG pass, once in the TTG->LLVM pass) now lives in
// exactly one place.
//
// Three rules decide how a tensor is spread over the block:
//
//   tile[d]      = sizePerThread[d] * threadsPerWarp[d] * warpsPerCTA[d]
//   repeats[d]   = shape[d] / tile[d]          (tile repetition, order-major)
//   threads[d]   = threadsPerWarp[d] * warpsPerCTA[d]
//
// A dimension of extent 1 is *replicated*: it has one repeat, its thread
// coordinate is 0, and every thread holds the same value.  This is what lets
// `tensor<8x1>` feed an 8x8 broadcast without inventing a layout for a
// dimension that has no extent.
//
// A dimension smaller than one tile is *wrapped*: only `shape[d]` of the
// `threads[d]` threads are distinct, the rest repeat the same coordinate
// (`tid % extent`).  Chapter 7 only uses that form for small `tl.arange`
// results that are immediately expanded, and the lowering documents it.
//
//===----------------------------------------------------------------------===//

#ifndef TINY_TRITON_CONVERSION_LAYOUTGEOMETRY_H
#define TINY_TRITON_CONVERSION_LAYOUTGEOMETRY_H

#include "tiny/Dialect/TinyTTG/TinyTTGAttrs.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/BuiltinTypes.h"

namespace tiny {

/// How many tiles a tensor repeats along `dim`.  Extent 1 and extents
/// smaller than one tile both count as a single repeat.
inline int64_t layoutRepeats(const ttg::BlockedAttr &blocked,
                             llvm::ArrayRef<int64_t> shape, int64_t dim) {
  int64_t extent = shape[dim];
  int64_t tile = blocked.getTileSize(dim);
  if (extent <= 1 || extent < tile)
    return 1;
  return extent / tile;
}

/// Threads the layout uses along `dim`.  A dimension of extent 1 still
/// nominates its lanes; they hold copies.
inline int64_t layoutThreads(const ttg::BlockedAttr &blocked, int64_t dim) {
  return blocked.getThreadsPerWarp()[dim] * blocked.getWarpsPerCTA()[dim];
}

/// Elements one thread holds, including `sizePerThread`.
inline int64_t layoutElementsPerThread(const ttg::BlockedAttr &blocked,
                                       llvm::ArrayRef<int64_t> shape) {
  int64_t count = 1;
  for (int64_t dim = 0, rank = shape.size(); dim < rank; ++dim)
    count *= blocked.getSizePerThread()[dim] *
             layoutRepeats(blocked, shape, dim);
  return count;
}

/// Threads that take part in the layout.  Fewer than a block means the
/// remaining threads repeat the same coordinates.
inline int64_t layoutThreadsInLayout(const ttg::BlockedAttr &blocked) {
  int64_t count = 1;
  for (int64_t dim = 0, rank = blocked.getRank(); dim < rank; ++dim)
    count *= layoutThreads(blocked, dim);
  return count;
}

/// Position of one register inside the thread, split into the tile repeat
/// and the slot inside `sizePerThread`.
struct ElementPosition {
  llvm::SmallVector<int64_t> repeat;
  llvm::SmallVector<int64_t> within;
};

/// Enumerate a thread's registers the way `order` says: dimension
/// `order[0]` varies fastest.  For the rank-1 layouts of chapters 3-6 this is
/// exactly the `r * threads + tid` order they documented.
inline ElementPosition elementPosition(const ttg::BlockedAttr &blocked,
                                       llvm::ArrayRef<int64_t> shape,
                                       int64_t index) {
  int64_t rank = shape.size();
  ElementPosition position;
  position.repeat.assign(rank, 0);
  position.within.assign(rank, 0);

  int64_t rest = index;
  for (int64_t i = 0; i < rank; ++i) {
    int64_t dim = blocked.getOrder()[i];
    int64_t withinCount = blocked.getSizePerThread()[dim];
    position.within[dim] = rest % withinCount;
    rest /= withinCount;
    int64_t repeatCount = layoutRepeats(blocked, shape, dim);
    position.repeat[dim] = rest % repeatCount;
    rest /= repeatCount;
  }
  return position;
}

/// Inverse of `elementPosition`: the register index of a position.
inline int64_t linearElementIndex(const ttg::BlockedAttr &blocked,
                                  llvm::ArrayRef<int64_t> shape,
                                  const ElementPosition &position) {
  int64_t index = 0;
  int64_t stride = 1;
  for (int64_t i = 0, rank = shape.size(); i < rank; ++i) {
    int64_t dim = blocked.getOrder()[i];
    index += (position.within[dim] +
              position.repeat[dim] * blocked.getSizePerThread()[dim]) *
             stride;
    stride *= blocked.getSizePerThread()[dim] * layoutRepeats(blocked, shape, dim);
  }
  return index;
}

/// Stride of a dimension's thread coordinate in the linear thread index.
inline int64_t layoutThreadStride(const ttg::BlockedAttr &blocked,
                                  int64_t dim) {
  int64_t stride = 1;
  for (int64_t i = 0, rank = blocked.getRank(); i < rank; ++i) {
    int64_t current = blocked.getOrder()[i];
    if (current == dim)
      break;
    stride *= layoutThreads(blocked, current);
  }
  return stride;
}

} // namespace tiny

#endif // TINY_TRITON_CONVERSION_LAYOUTGEOMETRY_H
