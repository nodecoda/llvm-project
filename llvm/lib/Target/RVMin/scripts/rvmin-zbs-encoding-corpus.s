# Chapter 21: Zbs.  Assembled with -mattr=+zbs on both triples.
        bset    a0, a1, a2
        bclr    s0, s1, s2
        binv    t0, t1, t2
        bext    a3, a4, a5

        bseti   a0, a1, 5
        bclri   s0, s1, 0
        binvi   t0, t1, 31
        bexti   a3, a4, 20
