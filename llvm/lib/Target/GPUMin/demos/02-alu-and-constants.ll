; Demo 2: the integer ALU, including the two cases that need lowering work.
;
;   * 0x12345678 does not fit in 16 bits, so `movi` cannot carry it: the
;     selector emits a `movi` + shift pair (chapter 04).
;   * `ashr` must sign-extend, which is a different opcode from `shr`.
;
; out[t] = ((in[t] ^ 0x12345678) + 0xffff) >> 3 | (in[t] << 5) ^ (in[t] & 0xff)
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()

define void @alu(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %x = load i32, ptr addrspace(1) %pin

  %a = xor i32 %x, 305419896          ; 0x12345678
  %b = add i32 %a, 65535
  %c = ashr i32 %b, 3                 ; arithmetic shift
  %d = shl i32 %x, 5
  %e = or i32 %c, %d
  %f = and i32 %x, 255
  %r = xor i32 %e, %f

  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %r, ptr addrspace(1) %pout
  ret void
}

attributes #0 = { "gpumin_kernel" }
