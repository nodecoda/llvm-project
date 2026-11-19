//===- TargetInfo.cpp - GPUMin's thread model ------------------*- C++ -*-===//

#include "tiny/Conversion/TargetInfo.h"

using namespace mlir;

namespace tiny {

ttg::BlockedAttr TargetInfo::getDefaultBlocked(MLIRContext *context,
                                               int64_t rank) const {
  switch (rank) {
  case 1:
    return ttg::BlockedAttr::get(context, {1}, {lanesPerWarp}, {warpsPerCTA},
                                 {0});
  case 2:
    return ttg::BlockedAttr::get(context, {1, 1}, {1, lanesPerWarp},
                                 {warpsPerCTA, 1}, {1, 0});
  default:
    return {};
  }
}

} // namespace tiny
