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

  // Chapter 40: under a hard-float ABI, single-precision arguments use the
  // floating-point argument registers instead.  With F disabled these rules
  // are never reached: the type legalizer has already softened f32 to i32
  // before the calling-convention analysis runs.
  CCIfType<[f32], CCAssignToReg<[F10, F11, F12, F13, F14, F15, F16, F17]>>,

  // Everything else goes on the stack, 4-byte aligned.
  CCIfType<[i32], CCAssignToStack<4, 4>>,
  CCIfType<[f32], CCAssignToStack<4, 4>>
]>;

def RetCC_RVMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[X10, X11]>>,
  CCIfType<[f32], CCAssignToReg<[F10, F11]>>
]>;

// Callee-saved registers: the return address and s0-s11.  The same list feeds
// two consumers:
//
//   * PEI spills and restores these registers around a function that modifies
//     them (CSR_RVMin_SaveList),
//   * the register allocator gets the complementary information at call sites
//     through the call-preserved mask (CSR_RVMin_RegMask), which is what tells
//     it that a value in s0-s11 survives a call while everything else does not.
//
// `ra` is in the list even though a call overwrites it: saving it in the
// prologue and reloading it before the return is exactly how a non-leaf
// function keeps its own return address.
def CSR_RVMin : CalleeSavedRegs<(add X1, X8, X9, (sequence "X%u", 18, 27))>;

// Chapter 40: the floating-point callee-saved set is fs0-fs11 (f8, f9 and
// f18-f27 in architectural numbering).  ft0-ft11 and fa0-fa7 are
// caller-saved.  F and D share the same list.
def CSR_RVMin_FP : CalleeSavedRegs<(add CSR_RVMin, F8, F9,
                                       (sequence "F%u", 18, 27))>;
