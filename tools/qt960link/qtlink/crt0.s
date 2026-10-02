# crt0.s — qtlink's entry. NINDY's "go 10100000" reaches it with callx, on NINDY's own
# stack; qtlink keeps that stack, clears its .bss and calls main.
#
# main only returns when the link has failed, and then qtlink stops here: it does not
# ret to NINDY. NINDY takes a program back through a breakpoint trace fault (its exit
# stub is fmark; syncf; .word 0xfeedface, recognised by the fault handler at 0x5150 of
# NINDY 3.01), and MAME's i960 raises no trace faults. A plain ret leaves NINDY's
# "program running" flag set, and its command loop then returns out of the monitor.
	.section .text.start, "ax"
	.globl	start
start:
	lda	__bss_start, r4
	lda	__bss_end, r5
	mov	0, r6
1:	cmpo	r4, r5
	bge	2f
	st	r6, (r4)
	addo	4, r4, r4
	b	1b
2:	mov	0, g14			# gcc's calling convention: g14 is 0 at a call
	call	main
3:	b	3b			# stopped: reset the board for NINDY
