//===-- MiniRVRegisterInfo.td - MiniRV register file --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

class MiniRVReg<bits<5> num, string n> : Register<n> {
  let Namespace = "MiniRV";
  let HWEncoding{4-0} = num;
  let DwarfNumbers = [num];
}

foreach i = 0-31 in {
  def X#i : MiniRVReg<i, "x"#i>;
}

def GPR : RegisterClass<"MiniRV", [i64], 64, (sequence "X%u", 0, 31)>;

// The RISC-V psABI callee-saved set.  X1 is not preserved by a call in the
// ABI, but a function that makes a call has to keep its own return address
// somewhere, so listing it here makes the callee-saved machinery spill it
// exactly when the function clobbers it.
def CSR_MiniRV : CalleeSavedRegs<(add X1, (sequence "X%u", 8, 9),
                                     (sequence "X%u", 18, 27))>;
