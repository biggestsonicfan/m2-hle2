# stubs.s — the instructions qtlink checks. Each stub is called as
#   u32 stub(u32 g0, u32 g1, u32 g2, u32 g3)
# and answers in g0. qtlink copies the stubs, byte for byte, to the Model 2B (kernel
# RAM, QTLINK_REMOTE in qtlink.c), runs each one there with the kernel's CALL and here
# with a plain call, and compares the two g0s. A stub is position-independent: it only
# touches g0-g3, its own locals and the stack above sp.
#
# A "-cc" stub answers the condition code (AC & 7) instead of the result.
#
# Table entry: name, first word, end, argument mode (qtlink.c, M_*).

	.set	M_ANY,   0		# g0-g3 random
	.set	M_DIV,   1		# g1 odd (never 0)
	.set	M_CARRY, 2		# g2 = 0 or 2 (the carry bit of AC)
	.set	M_COUNT, 4		# g1 = 0..39 (a shift count or bit number, past 31 too)
	.set	M_BIT,   8		# g1 = 0..31, g2 = 0..31
	.set	M_CC,   16		# g2 = 0..7 (a condition code to start from)

	.macro	TEST name, mode
	.section .rodata.names, "a"
1:	.asciz	"\name"
	.section .rodata.tests, "a"
	.word	1b, 2f, 3f, \mode
	.section .stubs, "ax"
	.align	4
2:
	.endm

	.macro	DONE
	ret
3:
	.endm

	.section .rodata.tests, "a"
	.align	4
	.globl	tests
tests:

# ---- arithmetic ----------------------------------------------------------------
	TEST	"ADDO", M_ANY
	addo	g1, g0, g0
	DONE
	TEST	"SUBO", M_ANY
	subo	g1, g0, g0
	DONE
	TEST	"MULO", M_ANY
	mulo	g1, g0, g0
	DONE
	TEST	"DIVO", M_DIV
	divo	g1, g0, g0
	DONE
	TEST	"REMO", M_DIV
	remo	g1, g0, g0
	DONE
	TEST	"EMUL-LO", M_ANY
	emul	g1, g0, r4
	mov	r4, g0
	DONE
	TEST	"EMUL-HI", M_ANY
	emul	g1, g0, r4
	mov	r5, g0
	DONE
	TEST	"ADDC", M_CARRY
	modac	2, g2, r4
	addc	g1, g0, g0
	DONE
	TEST	"ADDC-CC", M_CARRY
	modac	2, g2, r4
	addc	g1, g0, g0
	modac	0, 0, g0
	and	7, g0, g0
	DONE
	TEST	"SUBC", M_CARRY
	modac	2, g2, r4
	subc	g1, g0, g0
	DONE
	TEST	"SUBC-CC", M_CARRY
	modac	2, g2, r4
	subc	g1, g0, g0
	modac	0, 0, g0
	and	7, g0, g0
	DONE

# ---- logic -----------------------------------------------------------------------
	TEST	"AND", M_ANY
	and	g1, g0, g0
	DONE
	TEST	"OR", M_ANY
	or	g1, g0, g0
	DONE
	TEST	"XOR", M_ANY
	xor	g1, g0, g0
	DONE
	TEST	"NAND", M_ANY
	nand	g1, g0, g0
	DONE
	TEST	"NOR", M_ANY
	nor	g1, g0, g0
	DONE
	TEST	"XNOR", M_ANY
	xnor	g1, g0, g0
	DONE
	TEST	"ANDNOT", M_ANY
	andnot	g1, g0, g0
	DONE
	TEST	"NOTAND", M_ANY
	notand	g1, g0, g0
	DONE
	TEST	"ORNOT", M_ANY
	ornot	g1, g0, g0
	DONE
	TEST	"NOTOR", M_ANY
	notor	g1, g0, g0
	DONE
	TEST	"NOT", M_ANY
	not	g0, g0
	DONE

# ---- shifts and bits -------------------------------------------------------------
	TEST	"SHLO", M_COUNT
	shlo	g1, g0, g0
	DONE
	TEST	"SHRO", M_COUNT
	shro	g1, g0, g0
	DONE
	TEST	"SHRI", M_COUNT
	shri	g1, g0, g0
	DONE
	TEST	"ROTATE", M_COUNT
	rotate	g1, g0, g0
	DONE
	TEST	"SETBIT", M_BIT
	setbit	g1, g0, g0
	DONE
	TEST	"CLRBIT", M_BIT
	clrbit	g1, g0, g0
	DONE
	TEST	"NOTBIT", M_BIT
	notbit	g1, g0, g0
	DONE
	TEST	"ALTERBIT", M_BIT | M_CC
	modac	7, g2, r4
	alterbit g1, g0, g0
	DONE
	TEST	"CHKBIT-CC", M_BIT
	chkbit	g1, g0
	modac	0, 0, g0
	and	7, g0, g0
	DONE
	TEST	"SCANBIT", M_ANY
	scanbit	g0, g0
	DONE
	TEST	"SPANBIT", M_ANY
	spanbit	g0, g0
	DONE
	TEST	"EXTRACT", M_BIT
	extract	g1, g2, g0
	DONE
	TEST	"MODIFY", M_ANY
	modify	g1, g0, g2
	mov	g2, g0
	DONE

# ---- compares ----------------------------------------------------------------
	TEST	"CMPO-CC", M_ANY
	cmpo	g0, g1
	modac	0, 0, g0
	and	7, g0, g0
	DONE
	TEST	"CMPI-CC", M_ANY
	cmpi	g0, g1
	modac	0, 0, g0
	and	7, g0, g0
	DONE
	TEST	"CONCMPO", M_CC
	modac	7, g2, r4
	concmpo	g0, g1
	modac	0, 0, g0
	and	7, g0, g0
	DONE
	TEST	"CONCMPI", M_CC
	modac	7, g2, r4
	concmpi	g0, g1
	modac	0, 0, g0
	and	7, g0, g0
	DONE
	TEST	"CMPINCO", M_ANY
	cmpinco	g0, g1, g0
	DONE

# ---- multi-word loads and stores (on the stack above sp) ---------------------------
	TEST	"STL+LD", M_ANY
	mov	g0, r4
	mov	g1, r5
	stl	r4, (sp)
	ld	4(sp), g0
	DONE
	TEST	"ST+LDL", M_ANY
	st	g0, (sp)
	st	g1, 4(sp)
	ldl	(sp), r4
	mov	r5, g0
	DONE
	TEST	"ST+LDQ", M_ANY
	st	g0, (sp)
	st	g1, 4(sp)
	st	g2, 8(sp)
	st	g3, 12(sp)
	ldq	(sp), r4
	mov	r7, g0
	DONE

	.section .rodata.tests, "a"
	.word	0, 0, 0, 0
