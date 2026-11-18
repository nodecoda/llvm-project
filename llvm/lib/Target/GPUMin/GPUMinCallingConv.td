//===-- GPUMinCallingConv.td - GPUMin calling conventions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The device-function half of the GM32 ABI.  Kernels do not use it (their
// arguments arrive through the launch descriptor described in the plan,
// section 4.5); ordinary functions pass values in r4-r11 and return in r4-r5.
//
//   arguments : r4-r11, then the private stack
//   returns   : r4-r5
//   callee-saved : r20-r27, lr (r31)
//
// lr follows the AArch64 model: it is both *reserved* (the register allocator
// never assigns it) and *callee-saved* (every function that calls saves the
// incoming return address in its own frame and restores it before ret).
// Reserving it is what keeps the allocator from holding a value in it across
// a call; listing it here is what makes PEI allocate the spill slot and the
// call-preserved mask describe the truth.
//
//===----------------------------------------------------------------------===//

def CC_GPUMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[R4, R5, R6, R7, R8, R9, R10, R11]>>,
  CCIfType<[i32], CCAssignToStack<4, 4>>
]>;

def RetCC_GPUMin : CallingConv<[
  CCIfType<[i32], CCAssignToReg<[R4, R5]>>
]>;

def CSR_GPUMin : CalleeSavedRegs<(add R31, (sequence "R%u", 20, 27))>;
