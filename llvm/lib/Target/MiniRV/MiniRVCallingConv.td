//===-- MiniRVCallingConv.td - Calling conventions for MiniRV --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A deliberately small convention: eight integer arguments in x10..x17, the
// rest on the stack, and one integer result in x10.  MiniRV has no call
// instruction, so a caller cannot actually be generated; the convention exists
// so that formal arguments and returns can be lowered.
//
//===----------------------------------------------------------------------===//

def CC_MiniRV : CallingConv<[
  CCIfType<[i32], CCPromoteToType<i64>>,
  CCIfType<[i64], CCAssignToReg<[X10, X11, X12, X13, X14, X15, X16, X17]>>,
  CCIfType<[i64], CCAssignToStack<8, 8>>
]>;

def RetCC_MiniRV : CallingConv<[
  CCIfType<[i32], CCPromoteToType<i64>>,
  CCIfType<[i64], CCAssignToReg<[X10]>>
]>;
