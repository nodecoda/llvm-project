; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+zbb -O2 %s -o - | FileCheck %s --check-prefix=ZBB
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+zbs -O2 %s -o - | FileCheck %s --check-prefix=ZBS
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 %s -o - | FileCheck %s --check-prefix=DEFAULT
;
; Chapter 21: the same IR produces single instructions with +zbb/+zbs and the
; chapter 10 expansion without them.  Inputs are function arguments so none of
; this is constant-folded away.

declare i32 @llvm.ctlz.i32(i32, i1)
declare i32 @llvm.cttz.i32(i32, i1)
declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.bswap.i32(i32)
declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i32 @llvm.fshr.i32(i32, i32, i32)

define i32 @clz(i32 %x) {
  %r = call i32 @llvm.ctlz.i32(i32 %x, i1 false)
  ret i32 %r
}

define i32 @ctz(i32 %x) {
  %r = call i32 @llvm.cttz.i32(i32 %x, i1 false)
  ret i32 %r
}

define i32 @cpop(i32 %x) {
  %r = call i32 @llvm.ctpop.i32(i32 %x)
  ret i32 %r
}

define i32 @rev8(i32 %x) {
  %r = call i32 @llvm.bswap.i32(i32 %x)
  ret i32 %r
}

define i32 @andn(i32 %x, i32 %y) {
  %n = xor i32 %y, -1
  %r = and i32 %x, %n
  ret i32 %r
}

define i32 @orn(i32 %x, i32 %y) {
  %n = xor i32 %y, -1
  %r = or i32 %x, %n
  ret i32 %r
}

define i32 @xnor(i32 %x, i32 %y) {
  %n = xor i32 %y, -1
  %r = xor i32 %x, %n
  ret i32 %r
}

define i32 @smin(i32 %x, i32 %y) {
  %c = icmp slt i32 %x, %y
  %r = select i1 %c, i32 %x, i32 %y
  ret i32 %r
}

define i32 @umax(i32 %x, i32 %y) {
  %c = icmp ugt i32 %x, %y
  %r = select i1 %c, i32 %x, i32 %y
  ret i32 %r
}

define i32 @rol(i32 %x, i32 %y) {
  %r = call i32 @llvm.fshl.i32(i32 %x, i32 %x, i32 %y)
  ret i32 %r
}

define i32 @rori(i32 %x) {
  %r = call i32 @llvm.fshr.i32(i32 %x, i32 %x, i32 5)
  ret i32 %r
}

define i32 @zext_h(i32 %x) {
  %r = and i32 %x, 65535
  ret i32 %r
}

define i32 @sext_b(i32 %x) {
  %t = trunc i32 %x to i8
  %r = sext i8 %t to i32
  ret i32 %r
}

; Zbs idioms: 1 << n, x | (1 << n), x & ~(1 << n), (x >> n) & 1.
define i32 @bset(i32 %x, i32 %s) {
  %b = shl i32 1, %s
  %r = or i32 %x, %b
  ret i32 %r
}

define i32 @bclr(i32 %x, i32 %s) {
  %b = shl i32 1, %s
  %n = xor i32 %b, -1
  %r = and i32 %x, %n
  ret i32 %r
}

define i32 @binv(i32 %x, i32 %s) {
  %b = shl i32 1, %s
  %r = xor i32 %x, %b
  ret i32 %r
}

define i32 @bext(i32 %x, i32 %s) {
  %sh = lshr i32 %x, %s
  %r = and i32 %sh, 1
  ret i32 %r
}

; Zbb turns each intrinsic into one instruction.
; ZBB-LABEL: clz:
; ZBB: {{^}}	clz{{.*}}
; ZBB-LABEL: ctz:
; ZBB: {{^}}	ctz{{.*}}
; ZBB-LABEL: cpop:
; ZBB: {{^}}	cpop{{.*}}
; ZBB-LABEL: rev8:
; ZBB: {{^}}	rev8{{.*}}
; ZBB-LABEL: andn:
; ZBB: {{^}}	andn{{.*}}
; ZBB-LABEL: orn:
; ZBB: {{^}}	orn{{.*}}
; ZBB-LABEL: xnor:
; ZBB: {{^}}	xnor{{.*}}
; ZBB-LABEL: smin:
; ZBB: {{^}}	min{{.*}}
; ZBB-LABEL: umax:
; ZBB: {{^}}	maxu{{.*}}
; ZBB-LABEL: rol:
; ZBB: {{^}}	rol{{.*}}
; ZBB-LABEL: rori:
; ZBB: {{^}}	rori{{.*}}
; ZBB-LABEL: zext_h:
; ZBB: {{^}}	zext.h{{.*}}
; ZBB-LABEL: sext_b:
; ZBB: {{^}}	sext.b{{.*}}

; Zbs single-bit idioms.
; ZBS-LABEL: bset:
; ZBS: {{^}}	bset{{.*}}
; ZBS-LABEL: bclr:
; ZBS: {{^}}	bclr{{.*}}
; ZBS-LABEL: binv:
; ZBS: {{^}}	binv{{.*}}
; ZBS-LABEL: bext:
; ZBS: {{^}}	bext{{.*}}

; The default subtarget keeps the chapter 10 expansions: none of the
; extension mnemonics may appear as an instruction.
; DEFAULT-NOT: {{^}}	clz{{.*}}
; DEFAULT-NOT: {{^}}	ctz{{.*}}
; DEFAULT-NOT: {{^}}	cpop{{.*}}
; DEFAULT-NOT: {{^}}	rev8{{.*}}
; DEFAULT-NOT: {{^}}	andn{{.*}}
; DEFAULT-NOT: {{^}}	orn{{.*}}
; DEFAULT-NOT: {{^}}	xnor{{.*}}
; DEFAULT-NOT: {{^}}	zext.h{{.*}}
; DEFAULT-NOT: {{^}}	sext.b{{.*}}
; DEFAULT-NOT: {{^}}	rol{{.*}}
; DEFAULT-NOT: {{^}}	rori{{.*}}
; DEFAULT-NOT: {{^}}	bset{{.*}}
; DEFAULT-NOT: {{^}}	bclr{{.*}}
; DEFAULT-NOT: {{^}}	binv{{.*}}
; DEFAULT-NOT: {{^}}	bext{{.*}}
