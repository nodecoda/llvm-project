//===- IntrinsicsGPUMin.td - Defines GPUMin intrinsics ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the GPUMin-specific intrinsics.  They are the IR entry
// point for the SIMT state a lane can read but not write: lane/warp/block
// identity and the active mask.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "gpumin" in { // All intrinsics start with "llvm.gpumin.".

  // Lane index inside its warp (0 .. warpsize-1).
  def int_gpumin_laneid : Intrinsic<[llvm_i32_ty], [],
                                    [IntrNoMem, IntrSpeculatable,
                                     IntrWillReturn]>;

  // Warp index inside its block (0 .. nwarps-1).
  def int_gpumin_warpid : Intrinsic<[llvm_i32_ty], [],
                                    [IntrNoMem, IntrSpeculatable,
                                     IntrWillReturn]>;

  // Block index inside the grid.
  def int_gpumin_blockid : Intrinsic<[llvm_i32_ty], [],
                                     [IntrNoMem, IntrSpeculatable,
                                      IntrWillReturn]>;

  // Thread index inside its block.
  def int_gpumin_tid : Intrinsic<[llvm_i32_ty], [],
                                 [IntrNoMem, IntrSpeculatable, IntrWillReturn]>;

  // Number of threads in a block.
  def int_gpumin_ntid : Intrinsic<[llvm_i32_ty], [],
                                  [IntrNoMem, IntrSpeculatable,
                                   IntrWillReturn]>;

  // Number of warps in a block.
  def int_gpumin_nwarps : Intrinsic<[llvm_i32_ty], [],
                                    [IntrNoMem, IntrSpeculatable,
                                     IntrWillReturn]>;

  // Address of the launch descriptor's argument block.  The launcher writes
  // it into the uniform register ur0; the read is uniform across the warp.
  def int_gpumin_args : Intrinsic<[llvm_i32_ty], [],
                                  [IntrNoMem, IntrSpeculatable,
                                   IntrWillReturn]>;

  // One bit per lane: bit i is set when lane i is active in the current warp.
  // Deliberately *not* IntrSpeculatable: the mask changes when a warp
  // diverges (chapter 14), so hoisting the read out of a branch would observe
  // the pre-divergence mask.  IntrNoMem still lets two reads in the same
  // region CSE.
  def int_gpumin_activemask : Intrinsic<[llvm_i32_ty], [],
                                        [IntrNoMem, IntrWillReturn]>;
}
