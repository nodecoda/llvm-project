# Chapter 13: the M extension.  These eight instructions share funct7
# 0b0000001 and are only accepted when the subtarget has the M feature, so
# this file is assembled with -mattr=+m on both triples.
        mul a0, a1, a2
        mulh a0, a1, a2
        mulhsu a0, a1, a2
        mulhu a0, a1, a2
        div a0, a1, a2
        divu a0, a1, a2
        rem a0, a1, a2
        remu a0, a1, a2

        mul s0, s1, s2
        mulhsu t0, t1, t2
        divu ra, sp, gp
        remu a7, x0, x1
