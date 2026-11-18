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

  // Chapter 29: x/y block coordinates inside a 2D grid.  sr.blockid stays the
  // linear index (blockidy * gridX + blockidx), so 1D kernels are unchanged.
  def int_gpumin_blockidx : Intrinsic<[llvm_i32_ty], [],
                                      [IntrNoMem, IntrSpeculatable,
                                       IntrWillReturn]>;
  def int_gpumin_blockidy : Intrinsic<[llvm_i32_ty], [],
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

  // Base address of this block's shared-memory window, in address space 2.
  // Static shared objects get fixed offsets from it (chapter 16), so a kernel
  // never hard-codes the window base; the MVP launcher places it at 0.
  def int_gpumin_shared : Intrinsic<[llvm_i32_ty], [],
                                    [IntrNoMem, IntrSpeculatable,
                                     IntrWillReturn]>;

  // Chapter 31: f32 base-2 exponential, the SFU primitive behind softmax.
  // The instruction is a unary R-type in the 0x0B SFU opcode.
  def int_gpumin_exp2 : Intrinsic<[llvm_float_ty], [llvm_float_ty],
                                  [IntrNoMem, IntrSpeculatable,
                                   IntrWillReturn]>;

  // Chapter 17: block-wide barrier.  A call returns only when every warp in
  // the block has reached it, which is what makes shared-memory exchanges
  // across warps well defined.  It has no value, so the node is pure chain.
  def int_gpumin_barrier : Intrinsic<[], [], [IntrHasSideEffects,
                                             IntrConvergent]>;

  // One bit per lane: bit i is set when lane i is active in the current warp.
  // The value is *not* a function of the operands (there are none): it depends
  // on which lanes are executing, so two reads on either side of a branch can
  // differ.  Chapter 14 therefore marks the read as effectful, which is what
  // keeps CSE -- at the DAG level as well as the machine level -- from
  // merging those reads into one that observes the pre-divergence mask.
  def int_gpumin_activemask : Intrinsic<[llvm_i32_ty], [],
                                        [IntrHasSideEffects, IntrWillReturn]>;
}
