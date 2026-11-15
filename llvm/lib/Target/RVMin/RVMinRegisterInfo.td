//===-- RVMinRegisterInfo.td - RVMin register definitions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Alternate register-name indices.  Index 0 is the "no alternate name" case
// handled by TableGen; RVMin uses the ABI names (zero/ra/sp/...) by default in
// assembly output and accepts them in the parser.
let FallbackRegAltNameIndex = NoRegAltName in
def ABIRegAltName : RegAltNameIndex;

class RVMinReg<bits<5> num, string n, list<string> alt = []> : Register<n> {
  field bits<5> Num = num;
  let Namespace = "RVMin";
  let HWEncoding{4-0} = num;
  let AltNames = alt;
  let DwarfNumbers = [num];
}

// RegAltNameIndices maps every register's AltNames list onto the index above.
// RVMin only exposes one alternate name per register: its ABI name.
let RegAltNameIndices = [ABIRegAltName] in {

def X0  : RVMinReg<0,  "x0", ["zero"]>;
def X1  : RVMinReg<1,  "x1", ["ra"]>;
def X2  : RVMinReg<2,  "x2", ["sp"]>;
def X3  : RVMinReg<3,  "x3", ["gp"]>;
def X4  : RVMinReg<4,  "x4", ["tp"]>;
def X5  : RVMinReg<5,  "x5", ["t0"]>;
def X6  : RVMinReg<6,  "x6", ["t1"]>;
def X7  : RVMinReg<7,  "x7", ["t2"]>;
def X8  : RVMinReg<8,  "x8", ["s0"]>;
def X9  : RVMinReg<9,  "x9", ["s1"]>;
def X10 : RVMinReg<10, "x10", ["a0"]>;
def X11 : RVMinReg<11, "x11", ["a1"]>;
def X12 : RVMinReg<12, "x12", ["a2"]>;
def X13 : RVMinReg<13, "x13", ["a3"]>;
def X14 : RVMinReg<14, "x14", ["a4"]>;
def X15 : RVMinReg<15, "x15", ["a5"]>;
def X16 : RVMinReg<16, "x16", ["a6"]>;
def X17 : RVMinReg<17, "x17", ["a7"]>;
def X18 : RVMinReg<18, "x18", ["s2"]>;
def X19 : RVMinReg<19, "x19", ["s3"]>;
def X20 : RVMinReg<20, "x20", ["s4"]>;
def X21 : RVMinReg<21, "x21", ["s5"]>;
def X22 : RVMinReg<22, "x22", ["s6"]>;
def X23 : RVMinReg<23, "x23", ["s7"]>;
def X24 : RVMinReg<24, "x24", ["s8"]>;
def X25 : RVMinReg<25, "x25", ["s9"]>;
def X26 : RVMinReg<26, "x26", ["s10"]>;
def X27 : RVMinReg<27, "x27", ["s11"]>;
def X28 : RVMinReg<28, "x28", ["t3"]>;
def X29 : RVMinReg<29, "x29", ["t4"]>;
def X30 : RVMinReg<30, "x30", ["t5"]>;
def X31 : RVMinReg<31, "x31", ["t6"]>;

} // RegAltNameIndices = [ABIRegAltName]

// Allocation order follows the ILP32 ABI: caller-saved argument/temporary
// registers first, callee-saved registers next, and the special registers
// (zero/ra/sp/gp/tp) last so they are only picked under pressure.
def GPR : RegisterClass<"RVMin", [i32], 32,
  (add (sequence "X%u", 10, 17),
       (sequence "X%u", 5, 7),
       (sequence "X%u", 28, 31),
       (sequence "X%u", 8, 9),
       (sequence "X%u", 18, 27),
       X0, X1, X2, X3, X4)>;
