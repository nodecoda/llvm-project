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

// Chapter 47: the vector register file.  Zve32x has 32 vector registers and
// a fixed VLEN of 128 bits in this course, so one register holds a <4 x i32>.
// The registers are not part of the scalar ABI; code generation initially
// rejects vector arguments and returns (chapter 48).
class RVMinVReg<bits<5> num, string n> : Register<n> {
  field bits<5> Num = num;
  let Namespace = "RVMin";
  let HWEncoding{4-0} = num;
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

// Chapter 47: v0-v31.  v0 is reserved as the mask register in the full V
// extension; this subset never emits a mask, so all 32 registers are usable
// as ordinary vector values.
def V0  : RVMinVReg<0,  "v0">;
def V1  : RVMinVReg<1,  "v1">;
def V2  : RVMinVReg<2,  "v2">;
def V3  : RVMinVReg<3,  "v3">;
def V4  : RVMinVReg<4,  "v4">;
def V5  : RVMinVReg<5,  "v5">;
def V6  : RVMinVReg<6,  "v6">;
def V7  : RVMinVReg<7,  "v7">;
def V8  : RVMinVReg<8,  "v8">;
def V9  : RVMinVReg<9,  "v9">;
def V10 : RVMinVReg<10, "v10">;
def V11 : RVMinVReg<11, "v11">;
def V12 : RVMinVReg<12, "v12">;
def V13 : RVMinVReg<13, "v13">;
def V14 : RVMinVReg<14, "v14">;
def V15 : RVMinVReg<15, "v15">;
def V16 : RVMinVReg<16, "v16">;
def V17 : RVMinVReg<17, "v17">;
def V18 : RVMinVReg<18, "v18">;
def V19 : RVMinVReg<19, "v19">;
def V20 : RVMinVReg<20, "v20">;
def V21 : RVMinVReg<21, "v21">;
def V22 : RVMinVReg<22, "v22">;
def V23 : RVMinVReg<23, "v23">;
def V24 : RVMinVReg<24, "v24">;
def V25 : RVMinVReg<25, "v25">;
def V26 : RVMinVReg<26, "v26">;
def V27 : RVMinVReg<27, "v27">;
def V28 : RVMinVReg<28, "v28">;
def V29 : RVMinVReg<29, "v29">;
def V30 : RVMinVReg<30, "v30">;
def V31 : RVMinVReg<31, "v31">;

// Chapter 53: LMUL=2.  A v8i32 value occupies a pair of 128-bit registers.
// The sub-register indices describe the two halves of the 256-bit group; the
// super-register's HWEncoding is the first half's number, which is what the
// instruction encoding uses (vadd.vv v8, ... with vtype m2).
def sub_vrm1_0 : SubRegIndex<128>;
def sub_vrm1_1 : SubRegIndex<128, 128>;

class RVMinVM2Reg<bits<5> num, string n, Register lo, Register hi>
    : RegisterWithSubRegs<n, [lo, hi]> {
  field bits<5> Num = num;
  let Namespace = "RVMin";
  let HWEncoding{4-0} = num;
  let SubRegIndices = [sub_vrm1_0, sub_vrm1_1];
  let CoveredBySubRegs = 1;
}

foreach Index = !range(0, 32, 2) in {
  def V#Index#M2 : RVMinVM2Reg<Index, "v"#Index,
      !cast<Register>("V"#Index), !cast<Register>("V"#!add(Index, 1))>;
}

// Chapter 47: the vector register class.  VLEN is fixed at 128 bits, so the
// class type is v4i32.  Allocation starts at v8 and wraps back to v0-v7, which
// keeps v0 (the mask register in full RVV) out of the common path and matches
// the upstream RISC-V allocation order.
//
// Chapter 52: v4i1 is the mask value type.  It lives in the same 128-bit
// registers as v4i32 (the mask register only uses the low four bits), which
// keeps the type legal and lets SelectionDAG build it without promoting each
// mask element through the integer legalizer.
// Chapter 54: v4f32 is the same 128-bit view for vector float; it is only
// registered with the type legalizer when both V and F are enabled.
def VR : RegisterClass<"RVMin", [v4i32, v4i1, v4f32], 128,
  (add (sequence "V%u", 8, 31),
       (sequence "V%u", 0, 7))> {
  let DecoderMethod = "DecodeVRRegisterClass";
}

// Chapter 52: the architectural mask register.  This class contains only v0
// and exists to constrain the mask operand of vmerge; masks produced by
// ordinary vector code still live in VR, so the register allocator inserts a
// copy into v0 when the instruction needs it.
def VMV0 : RegisterClass<"RVMin", [v4i1], 128, (add V0)> {
  let DecoderMethod = "DecodeVRRegisterClass";
}

// Chapter 53: the LMUL=2 class.  A register is 256 bits (two 128-bit vector
// registers), but the spill slot only needs 16-byte alignment: the unit-stride
// load/store is element-aligned, and keeping the frame at 16 bytes matches the
// stack alignment RVMin already guarantees.  Allocation order mirrors VR.
def VRM2 : RegisterClass<"RVMin", [v8i32], 128,
  (add (sequence "V%uM2", 8, 31, 2),
       (sequence "V%uM2", 0, 6, 2))> {
  let Size = 256;
  let DecoderMethod = "DecodeVRM2RegisterClass";
}

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

// Chapter 44: value subsets of GPR used only by CompressPat.  A pattern that
// names one of these rejects the registers the compressed encoding cannot
// express (x0, and x2 for c.lui) while the instruction itself keeps using the
// plain GPR class, so the assembler is unaffected.
def GPRNoX0 : RegisterClass<"RVMin", [i32], 32, (sub GPR, X0)>;
def GPRNoX0X2 : RegisterClass<"RVMin", [i32], 32, (sub GPR, X0, X2)>;

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

