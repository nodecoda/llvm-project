# Chapter 41: the 26 D instructions, plus the two explicit round-toward-zero
# conversion forms that llc emits for fptosi/fptoui.  Every line must encode
# byte-identically under `-triple=rvmin -mattr=+f,+d` and the reference
# `-triple=riscv32 -mattr=+f,+d`.  D implies F, so `-mattr=+d` is enough on
# its own; the script spells both out to make the dependency visible.
fld fa0, 0(a0)
fsd fa1, 4(a1)
fmadd.d fa0, fa1, fa2, fa3
fmsub.d fa0, fa1, fa2, fa3
fnmsub.d fa0, fa1, fa2, fa3
fnmadd.d fa0, fa1, fa2, fa3
fadd.d fa0, fa1, fa2
fsub.d fa0, fa1, fa2
fmul.d fa0, fa1, fa2
fdiv.d fa0, fa1, fa2
fsqrt.d fa0, fa1
fsgnj.d fa0, fa1, fa2
fsgnjn.d fa0, fa1, fa2
fsgnjx.d fa0, fa1, fa2
fmin.d fa0, fa1, fa2
fmax.d fa0, fa1, fa2
fcvt.w.d a0, fa1
fcvt.wu.d a0, fa1
fcvt.d.w fa0, a1
fcvt.d.wu fa0, a1
fcvt.w.d a0, fa1, rtz
fcvt.wu.d a0, fa1, rtz
fcvt.s.d fa0, fa1
fcvt.d.s fa0, fa1
feq.d a0, fa1, fa2
flt.d a0, fa1, fa2
fle.d a0, fa1, fa2
fclass.d a0, fa1
