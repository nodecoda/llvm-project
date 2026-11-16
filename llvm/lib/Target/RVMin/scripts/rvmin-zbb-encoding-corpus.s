# Chapter 21: Zbb.  Every instruction is assembled with -mattr=+zbb on both
# triples and the two encodings are compared byte-for-byte.
        clz     a0, a1
        clz     x31, x30
        ctz     a0, a1
        cpop    s0, s1
        sext.b  t0, t1
        sext.h  a2, a3
        rev8    a4, a5
        zext.h  a6, a7

        rol     a0, a1, a2
        ror     s0, s1, s2
        rori    a0, a1, 5
        rori    a0, a1, 31

        andn    a0, a1, a2
        orn     s0, s1, s2
        xnor    t0, t1, t2

        min     a0, a1, a2
        minu    s0, s1, s2
        max     t0, t1, t2
        maxu    a3, a4, a5
