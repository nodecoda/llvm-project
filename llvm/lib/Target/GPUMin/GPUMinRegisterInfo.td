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
  (add (sequence "R%u", 4, 11),
       (sequence "R%u", 12, 19),
       (sequence "R%u", 20, 27),
       R28, R29, R0, R30, R31)>;

// A single-register class for the stack pointer; frame lowering and the
// register scavenger ask for it by name.
def SP : RegisterClass<"GPUMin", [i32], 32, (add R30)>;
