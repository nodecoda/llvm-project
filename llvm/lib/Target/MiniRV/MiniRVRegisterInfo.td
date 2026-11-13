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
