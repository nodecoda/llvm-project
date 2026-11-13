//===-- IntrinsicsRVTrim.td - RVTrim intrinsic definitions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Vendor intrinsics that have no upstream RISC-V counterpart.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "rvtrim" in {

  // <vscale x N x i32> @llvm.rvtrim.vfexp(<vscale x N x i32>)
  def int_rvtrim_vfexp : Intrinsic<[llvm_anyvector_ty], [LLVMMatchType<0>],
                                   [IntrNoMem, IntrSpeculatable]>;

} // TargetPrefix = "rvtrim"
