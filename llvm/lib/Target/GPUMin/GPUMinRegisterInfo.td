//===-- GPUMinRegisterInfo.td - GPUMin register definitions -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 00: the per-lane 32-bit general register file, r0-r31.
//
// GM32 fixes three of them by convention (plan section 4.1):
//
//   r0   the zero register: reads produce 0, writes are discarded
//   r30  the private-stack pointer (sp)
//   r31  the link register (lr)
//
// Chapters 01+ add the per-warp uniform file (ur0-ur7) and the per-lane
// predicate file (p0-p7).  They are separate register classes with their own
// types, which is why they are not part of GPR here.
//
//===----------------------------------------------------------------------===//

class GPUMinReg<bits<5> num, string n> : Register<n> {
  field bits<5> Num = num;
  let Namespace = "GPUMin";
  let HWEncoding{4-0} = num;
  let DwarfNumbers = [num];
}

foreach i = 0-31 in {
  def R#i : GPUMinReg<i, "r"#i>;
}

// The allocation order puts the caller-saved window first and keeps r0 (zero),
// r30 (sp) and r31 (lr) last.  Reserved registers are still reserved; the
// order only decides which registers the allocator prefers.
def GPR : RegisterClass<"GPUMin", [i32], 32,
  (add R1, R2, R3,
       (sequence "R%u", 4, 11),
       (sequence "R%u", 12, 19),
       (sequence "R%u", 20, 27),
       R28, R29, R0, R30, R31)> {
  let DecoderMethod = "DecodeGPRRegisterClass";
}

// A single-register class for the stack pointer; frame lowering and the
// register scavenger ask for it by name.
def SP : RegisterClass<"GPUMin", [i32], 32, (add R30)>;

//===----------------------------------------------------------------------===//
// Uniform registers (chapter 01)
//===----------------------------------------------------------------------===//
//
// ur0-ur7 are one register *per warp*, not per lane: every lane of a warp sees
// the same value.  That is what makes them useful for the kernel argument
// pointer, the block index and other warp-invariant values (chapter 13).
// They are a separate register class because a uniform value and a per-lane
// value are different LLVM types at the machine level.

class GPUMinUReg<bits<3> num, string n> : Register<n> {
  field bits<3> Num = num;
  let Namespace = "GPUMin";
  let HWEncoding{2-0} = num;
}

foreach i = 0-7 in {
  def UR#i : GPUMinUReg<i, "ur"#i>;
}

def UGPR : RegisterClass<"GPUMin", [i32], 32, (sequence "UR%u", 0, 7)> {
  let DecoderMethod = "DecodeUGPRRegisterClass";
}

//===----------------------------------------------------------------------===//
// Predicate registers (chapter 01)
//===----------------------------------------------------------------------===//
//
// p0-p7 hold one bit per lane.  They are written by SETP and consumed by the
// predicated forms; chapter 14 uses them to model divergence.

class GPUMinPReg<bits<3> num, string n> : Register<n> {
  field bits<3> Num = num;
  let Namespace = "GPUMin";
  let HWEncoding{2-0} = num;
}

foreach i = 0-7 in {
  def P#i : GPUMinPReg<i, "p"#i>;
}

def PRED : RegisterClass<"GPUMin", [i1], 1, (sequence "P%u", 0, 7)> {
  let DecoderMethod = "DecodePREDRegisterClass";
}
