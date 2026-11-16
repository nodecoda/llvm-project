# Chapter 22: A-extension encoding corpus.
#
# Every line is assembled twice by check-rvmin-encodings.sh -- once with
# -triple=rvmin and once with -triple=riscv32 -- and the encoding bytes must be
# identical.  The same file feeds the round-trip check in
# check-rvmin-disasm.sh.
#
# The aq/rl bits live in Inst{26:25}; the funct5 field in Inst{31:27} selects
# the operation.  LR.W leaves rs2 = 0, so its encoding has a zero in Inst{24:20}.
        lr.w        a0, (a1)
        lr.w.aq     a0, (a1)
        lr.w.rl     a0, (a1)
        lr.w.aqrl   a0, (a1)
        sc.w        a0, a2, (a1)
        sc.w.aq     a0, a2, (a1)
        sc.w.rl     a0, a2, (a1)
        sc.w.aqrl   a0, a2, (a1)
        amoswap.w   a0, a2, (a1)
        amoadd.w    a0, a2, (a1)
        amoxor.w    a0, a2, (a1)
        amoand.w    a0, a2, (a1)
        amoor.w     a0, a2, (a1)
        amomin.w    a0, a2, (a1)
        amomax.w    a0, a2, (a1)
        amominu.w   a0, a2, (a1)
        amomaxu.w   a0, a2, (a1)
        amoadd.w.aqrl a0, a2, (a1)
        fence
        fence.i
