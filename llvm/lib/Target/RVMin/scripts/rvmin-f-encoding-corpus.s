# Chapter 40: the 26 F instructions, plus the two explicit round-toward-zero
# conversion forms that llc emits for fptosi/fptoui.  Every line must encode
# byte-identically under `-triple=rvmin -mattr=+f` and the reference
# `-triple=riscv32 -mattr=+f`.
flw fa0, 0(a0)
fsw fa1, 4(a1)
fmadd.s fa0, fa1, fa2, fa3
fmsub.s fa0, fa1, fa2, fa3
fnmsub.s fa0, fa1, fa2, fa3
fnmadd.s fa0, fa1, fa2, fa3
fadd.s fa0, fa1, fa2
fsub.s fa0, fa1, fa2
fmul.s fa0, fa1, fa2
fdiv.s fa0, fa1, fa2
fsqrt.s fa0, fa1
fsgnj.s fa0, fa1, fa2
fsgnjn.s fa0, fa1, fa2
fsgnjx.s fa0, fa1, fa2
fmin.s fa0, fa1, fa2
fmax.s fa0, fa1, fa2
fcvt.w.s a0, fa1
fcvt.wu.s a0, fa1
fcvt.s.w fa0, a1
fcvt.s.wu fa0, a1
fcvt.w.s a0, fa1, rtz
fcvt.wu.s a0, fa1, rtz
fmv.x.w a0, fa1
fmv.w.x fa0, a1
feq.s a0, fa1, fa2
flt.s a0, fa1, fa2
fle.s a0, fa1, fa2
fclass.s a0, fa1
