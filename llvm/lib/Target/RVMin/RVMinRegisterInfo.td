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

// Chapter 43: compressed instructions encode most integer registers in three
// bits.  Only x8-x15 are addressable; the encoder stores the architectural
// number (8-15) and the 3-bit field keeps the low three bits, so x8 maps to 0
// and x15 to 7 exactly as the ISA requires.
def GPRC : RegisterClass<"RVMin", [i32], 32,
  (add X8, X9, X10, X11, X12, X13, X14, X15)> {
  let DecoderMethod = "DecodeGPRCRegisterClass";
}

// The stack pointer is implicit in the c.*sp instructions; keeping it as a
// one-register class lets the assembler accept and print `sp` without giving
// it an encoding field.
def SP : RegisterClass<"RVMin", [i32], 32, (add X2)>;

//===----------------------------------------------------------------------===//
// Chapter 40: F-extension registers
//===----------------------------------------------------------------------===//
//
// The floating-point register file is separate from the integer file.  RVMin
// models the single-precision view as its own 32-bit class; chapter 41 adds a
// 64-bit view over the same architectural registers, selected by the operand
// type of each instruction.
//
// The DWARF numbers start at 32 so that f0 and x0 do not collide.
class RVMinFPR<bits<5> num, int dwarf, string n, list<string> alt = []>
    : Register<n> {
  field bits<5> Num = num;
  let Namespace = "RVMin";
  let HWEncoding{4-0} = num;
  let AltNames = alt;
  let DwarfNumbers = [dwarf];
}

let RegAltNameIndices = [ABIRegAltName] in {

def F0:  RVMinFPR<0, 32, "f0", ["ft0"]>;
def F1:  RVMinFPR<1, 33, "f1", ["ft1"]>;
def F2:  RVMinFPR<2, 34, "f2", ["ft2"]>;
def F3:  RVMinFPR<3, 35, "f3", ["ft3"]>;
def F4:  RVMinFPR<4, 36, "f4", ["ft4"]>;
def F5:  RVMinFPR<5, 37, "f5", ["ft5"]>;
def F6:  RVMinFPR<6, 38, "f6", ["ft6"]>;
def F7:  RVMinFPR<7, 39, "f7", ["ft7"]>;
def F8:  RVMinFPR<8, 40, "f8", ["fs0"]>;
def F9:  RVMinFPR<9, 41, "f9", ["fs1"]>;
def F10: RVMinFPR<10,42,"f10", ["fa0"]>;
def F11: RVMinFPR<11,43,"f11", ["fa1"]>;
def F12: RVMinFPR<12,44,"f12", ["fa2"]>;
def F13: RVMinFPR<13,45,"f13", ["fa3"]>;
def F14: RVMinFPR<14,46,"f14", ["fa4"]>;
def F15: RVMinFPR<15,47,"f15", ["fa5"]>;
def F16: RVMinFPR<16,48,"f16", ["fa6"]>;
def F17: RVMinFPR<17,49,"f17", ["fa7"]>;
def F18: RVMinFPR<18,50,"f18", ["fs2"]>;
def F19: RVMinFPR<19,51,"f19", ["fs3"]>;
def F20: RVMinFPR<20,52,"f20", ["fs4"]>;
def F21: RVMinFPR<21,53,"f21", ["fs5"]>;
def F22: RVMinFPR<22,54,"f22", ["fs6"]>;
def F23: RVMinFPR<23,55,"f23", ["fs7"]>;
def F24: RVMinFPR<24,56,"f24", ["fs8"]>;
def F25: RVMinFPR<25,57,"f25", ["fs9"]>;
def F26: RVMinFPR<26,58,"f26", ["fs10"]>;
def F27: RVMinFPR<27,59,"f27", ["fs11"]>;
def F28: RVMinFPR<28,60,"f28", ["ft8"]>;
def F29: RVMinFPR<29,61,"f29", ["ft9"]>;
def F30: RVMinFPR<30,62,"f30", ["ft10"]>;
def F31: RVMinFPR<31,63,"f31", ["ft11"]>;

} // RegAltNameIndices = [ABIRegAltName]

// Chapter 41: the double-precision view of the same 32 architectural
// registers.  FLEN is 64 under D, so f32 values are NaN-boxed when they share
// a register with an f64 value; RVMin never reinterprets one as the other, and
// the two classes only differ in the instruction selected for a copy/spill.
// The allocation order deliberately matches FPR32 so register allocation looks
// the same in both views.
//
// This class is declared before FPR32 on purpose.  TableGen's generated
// getMinimalPhysRegClass() keeps the first class that contains a register when
// the classes are unrelated, and PEI uses that class to size callee-saved
// spill slots.  The 64-bit view gives every FP callee-saved register an 8-byte
// slot in both modes; a 4-byte slot under D would be overflowed by `fsd`.
def FPR64 : RegisterClass<"RVMin", [f64], 64,
  (add (sequence "F%u", 10, 17),
       (sequence "F%u", 0, 7),
       (sequence "F%u", 28, 31),
       F8, F9,
       (sequence "F%u", 18, 27))> {
  let DecoderMethod = "DecodeFPR64RegisterClass";
}

// Allocation order: caller-saved argument registers first (fa0-fa7), then the
// temporaries, then the callee-saved fs0-fs11 last.
def FPR32 : RegisterClass<"RVMin", [f32], 32,
  (add (sequence "F%u", 10, 17),
       (sequence "F%u", 0, 7),
       (sequence "F%u", 28, 31),
       F8, F9,
       (sequence "F%u", 18, 27))> {
  let DecoderMethod = "DecodeFPR32RegisterClass";
}

// Chapter 43: the compressed FP load/store forms address f8-f15 with three
// bits, exactly like GPRC.
def FPR32C : RegisterClass<"RVMin", [f32], 32,
  (add F8, F9, F10, F11, F12, F13, F14, F15)> {
  let DecoderMethod = "DecodeFPR32CRegisterClass";
}

def FPR64C : RegisterClass<"RVMin", [f64], 64,
  (add F8, F9, F10, F11, F12, F13, F14, F15)> {
  let DecoderMethod = "DecodeFPR64CRegisterClass";
}

