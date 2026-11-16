	.attribute	4, 16
	.attribute	5, "rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0_zcd1p0_xdemo1p0"
	.file	"demo.c"
	.text
	.globl	xdemo_madd                      # -- Begin function xdemo_madd
	.p2align	1
	.type	xdemo_madd,@function
xdemo_madd:                             # @xdemo_madd
# %bb.0:                                # %entry
	xdemo.madd	a0, a1, a0, a2
	ret
.Lfunc_end0:
	.size	xdemo_madd, .Lfunc_end0-xdemo_madd
                                        # -- End function
	.globl	xdemo_madd_commuted             # -- Begin function xdemo_madd_commuted
	.p2align	1
	.type	xdemo_madd_commuted,@function
xdemo_madd_commuted:                    # @xdemo_madd_commuted
# %bb.0:                                # %entry
	xdemo.madd	a0, a1, a0, a2
	ret
.Lfunc_end1:
	.size	xdemo_madd_commuted, .Lfunc_end1-xdemo_madd_commuted
                                        # -- End function
	.globl	xdemo_dot2                      # -- Begin function xdemo_dot2
	.p2align	1
	.type	xdemo_dot2,@function
xdemo_dot2:                             # @xdemo_dot2
# %bb.0:                                # %entry
	mul	a0, a1, a0
	xdemo.madd	a0, a3, a2, a0
	ret
.Lfunc_end2:
	.size	xdemo_dot2, .Lfunc_end2-xdemo_dot2
                                        # -- End function
	.ident	"clang version 24.0.0git (https://github.com/nodecoda/llvm-project.git 756265e4ce650c26861c4850668e6d42022edcbc)"
	.section	".note.GNU-stack","",@progbits
	.addrsig
