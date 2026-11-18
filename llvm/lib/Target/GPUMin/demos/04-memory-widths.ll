; Demo 4: the five load and three store widths.
;
; One lane writes its input as a byte and as a halfword, then reads both back
; three ways: zero-extended, sign-extended, and 32-bit.  The four result
; arrays make the sign/zero distinction visible in the output.
;
; Layout: in @ 0x000, bytes @ 0x200, halves @ 0x300, results @ 0x400
;         (u8, s8, u16, s16 -- 8 words each).
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()

define void @widths(ptr addrspace(1) %in, ptr addrspace(1) %bytes,
                    ptr addrspace(1) %halves, ptr addrspace(1) %res) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %x = load i32, ptr addrspace(1) %pin

  %pb = getelementptr i8, ptr addrspace(1) %bytes, i32 %t
  %b = trunc i32 %x to i8
  store i8 %b, ptr addrspace(1) %pb

  %ph = getelementptr i16, ptr addrspace(1) %halves, i32 %t
  %h = trunc i32 %x to i16
  store i16 %h, ptr addrspace(1) %ph

  %ub = load i8, ptr addrspace(1) %pb
  %ub32 = zext i8 %ub to i32
  %pu = getelementptr i32, ptr addrspace(1) %res, i32 %t
  store i32 %ub32, ptr addrspace(1) %pu

  %sb = load i8, ptr addrspace(1) %pb
  %sb32 = sext i8 %sb to i32
  %ps = getelementptr i32, ptr addrspace(1) %res, i32 %t
  %ps8 = getelementptr i32, ptr addrspace(1) %ps, i32 8
  store i32 %sb32, ptr addrspace(1) %ps8

  %uh = load i16, ptr addrspace(1) %ph
  %uh32 = zext i16 %uh to i32
  %pu16 = getelementptr i32, ptr addrspace(1) %ps, i32 16
  store i32 %uh32, ptr addrspace(1) %pu16

  %sh = load i16, ptr addrspace(1) %ph
  %sh32 = sext i16 %sh to i32
  %ps16 = getelementptr i32, ptr addrspace(1) %ps, i32 24
  store i32 %sh32, ptr addrspace(1) %ps16
  ret void
}

attributes #0 = { "gpumin_kernel" }
