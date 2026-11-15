//===-- RVMinCallingConv.td - RVMin calling conventions ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ILP32 argument passing, as much as chapters 3-7 need:
//
//   integer arguments : a0-a7 (x10-x17), then the stack
//   integer returns   : a0-a1 (x10-x11)
//
// Chapter 8 extends this with the full callee-saved set and calls.  Keeping the
// tables here (instead of hard-coding the analysis in C++) means the ABI is
// described in one declarative place.
//
//===----------------------------------------------------------------------===//

def CC_RVMin : CallingConv<[
  // Integer arguments are passed in a0-a7.
  CCIfType<[i32], CCAssignToReg<[X10, X11, X12, X13, X14, X15, X16, X17]>>,

  // Everything else goes on the stack, 4-byte aligned.
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def RetCC_RVMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[X10, X11]>>
]>;
