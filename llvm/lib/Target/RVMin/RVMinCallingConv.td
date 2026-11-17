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

// Chapter 51: the soft-float ABI (`ilp32` with hardware F/D) passes floating
// values as their integer bit patterns:
//
//   f32 -> one i32 (a0-a7, then a 4-byte stack slot)
//   f64 -> two i32 locations (an even-aligned aN/aN+1 pair, or an 8-byte
//          stack slot).  RV32D has no fmv.x.d, so the backend splits/joins
//          the halves through a private 8-byte stack object.
//
// The calling convention is selected at run time (RVMinIsSoftFloatABI) because
// the CC table itself is static while the ABI is a module property.  The
// CCBitConvertToType rule only changes LocVT/LocInfo; the lowering code does
// the actual bitcast.
def CC_RVMin_SoftFloat : CallingConv<[
  CCIfType<[f32], CCBitConvertToType<i32>>,
  CCIfType<[f64], CCCustom<"RVMinCC_AssignF64AsTwoI32">>,
  CCIfType<[i32], CCAssignToReg<[X10, X11, X12, X13, X14, X15, X16, X17]>>,
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def RetCC_RVMin_SoftFloat : CallingConv<[
  CCIfType<[f32], CCBitConvertToType<i32>>,
  CCIfType<[f64], CCCustom<"RVMinCC_AssignF64Return">>,
  CCIfType<[i32], CCAssignToReg<[X10, X11]>>
]>;

// Chapter 55: LMUL=1 vector arguments.  The subtable is reached only when V is
// enabled, so a module without V keeps the old scalar/sret path and never
// assigns a vector value to an architectural vector register.
//
//   vector arguments : v8-v23 (16 registers), then a 16-byte stack slot
//   vector returns   : v8
//   mask register    : v0 is never an argument register
def CC_RVMin_V : CallingConv<[
  CCIfType<[v4i32], CCAssignToReg<[V8, V9, V10, V11, V12, V13, V14, V15,
                                  V16, V17, V18, V19, V20, V21, V22, V23]>>,
  CCIfType<[v4i32], CCAssignToStack<16, 16>>
]>;

def RetCC_RVMin_V : CallingConv<[
  CCIfType<[v4i32], CCAssignToReg<[V8]>>
]>;

def CC_RVMin : CallingConv<[
  // Chapter 51: an `ilp32` module with hardware F/D uses the integer
  // convention for floating values.  All other ABIs fall through to the
  // hard-float rules below.
  CCIf<"RVMinIsSoftFloatABI(State)", CCDelegateTo<CC_RVMin_SoftFloat>>,

  // Chapter 55: vector arguments, but only when the subtarget has V.
  CCIf<"RVMinHasStdExtV(State)", CCDelegateTo<CC_RVMin_V>>,

  // Integer arguments are passed in a0-a7.
  CCIfType<[i32], CCAssignToReg<[X10, X11, X12, X13, X14, X15, X16, X17]>>,

  // Chapter 40: under a hard-float ABI, single-precision arguments use the
  // floating-point argument registers instead.  With F disabled these rules
  // are never reached: the type legalizer has already softened f32 to i32
  // before the calling-convention analysis runs.
  CCIfType<[f32], CCAssignToReg<[F10, F11, F12, F13, F14, F15, F16, F17]>>,
  // Chapter 41: under ILP32D a double argument uses the same fa0-fa7 window.
  CCIfType<[f64], CCAssignToReg<[F10, F11, F12, F13, F14, F15, F16, F17]>>,

  // Everything else goes on the stack, 4-byte aligned.
  CCIfType<[i32], CCAssignToStack<4, 4>>,
  CCIfType<[f32], CCAssignToStack<4, 4>>,
  CCIfType<[f64], CCAssignToStack<8, 8>>
]>;

def RetCC_RVMin : CallingConv<[
  CCIf<"RVMinIsSoftFloatABI(State)", CCDelegateTo<RetCC_RVMin_SoftFloat>>,
  CCIf<"RVMinHasStdExtV(State)", CCDelegateTo<RetCC_RVMin_V>>,
  CCIfType<[i32], CCAssignToReg<[X10, X11]>>,
  CCIfType<[f32], CCAssignToReg<[F10, F11]>>,
  CCIfType<[f64], CCAssignToReg<[F10, F11]>>
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

// Chapter 55: the vector ABI's callee-saved window is v1-v7 and v24-v31.
// v0 is the mask register and v8-v23 are caller-saved argument registers.
// The LMUL=2 aliases are deliberately not listed: PEI's alias-aware
// isPhysRegModified() marks both halves of e.g. V2M2, and V2/V3 are both in
// this list, so the generic spill/save path preserves the pair.
def CSR_RVMin_V : CalleeSavedRegs<(add CSR_RVMin,
                                   (sequence "V%u", 1, 7),
                                   (sequence "V%u", 24, 31))>;

// F and V can be enabled together; the callee-saved set is the union.
def CSR_RVMin_FP_V : CalleeSavedRegs<(add CSR_RVMin_FP,
                                      (sequence "V%u", 1, 7),
                                      (sequence "V%u", 24, 31))>;
