	.att_syntax
	.file	"sum.c"
	.text
	.globl	sum                             # -- Begin function sum
	.prefalign	4, .Lfunc_end0, nop
	.type	sum,@function
sum:                                    # @sum
	.cfi_startproc
# %bb.0:                                # %entry
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	movq	%rdi, -24(%rbp)
	movl	%esi, -12(%rbp)
	movl	$0, -8(%rbp)
	movl	$0, -4(%rbp)
.LBB0_1:                                # %for.cond
                                        # =>This Inner Loop Header: Depth=1
	movl	-4(%rbp), %eax
	movl	-12(%rbp), %ecx
	cmpl	%ecx, %eax
	setl	%al
	testb	$1, %al
	jne	.LBB0_2
	jmp	.LBB0_4
.LBB0_2:                                # %for.body
                                        #   in Loop: Header=BB0_1 Depth=1
	movq	-24(%rbp), %rax
	movl	-4(%rbp), %ecx
	movslq	%ecx, %rcx
	shlq	$2, %rcx
	leaq	(%rax,%rcx), %rax
	movl	(%rax), %eax
	addl	%eax, -8(%rbp)
# %bb.3:                                # %for.inc
                                        #   in Loop: Header=BB0_1 Depth=1
	incl	-4(%rbp)
	jmp	.LBB0_1
.LBB0_4:                                # %for.end
	movl	-8(%rbp), %eax
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	retq
.Lfunc_end0:
	.size	sum, .Lfunc_end0-sum
	.cfi_endproc
                                        # -- End function
	.ident	"clang version 24.0.0git (https://github.com/nodecoda/llvm-project.git 756265e4ce650c26861c4850668e6d42022edcbc)"
	.section	".note.GNU-stack","",@progbits
