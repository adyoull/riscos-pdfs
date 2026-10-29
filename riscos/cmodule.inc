@ cmodule.inc: veneers between RISC OS module entry points and C, for the
@ test bed's C modules. A module's header.s writes the module header (in
@ section .text.header, starting at module_start) and then includes this.
@
@ The C side (cmodule.h) provides:
@   oserror *mod_init(const char *tail, int instance, void *pw);
@   oserror *mod_final(int fatal);
@   oserror *mod_swi(unsigned number, regs *r);   r->r[0..9] in and out
@ and, when the header uses them:
@   void mod_service(regs *r);                    r->r[0..9] in and out
@   oserror *mod_command(int index, const char *tail, int argc);
@
@ C code is compiled -fpie, so its only absolute addresses are GOT entries.
@ cm_init adds the module's load address to each of them first (once per
@ load: got_base remembers the address they were last relocated for).
@
@ Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0
@ (the same code is in riscos-linux-testbed under the MIT licence).

	.macro	CLRV
	msr	APSR_nzcvq, #0
	.endm
	.macro	SETV
	msr	APSR_nzcvq, #0x10000000
	.endm

@ Include it from the header's section, so the header's offsets to these
@ veneers are plain differences within one section.
	.align	2

cm_init:
	stmfd	sp!, {r4-r11, lr}
	bl	cm_relocate_got
	mov	r0, r10
	mov	r1, r11
	mov	r2, r12
	bl	mod_init
	b	cm_return

cm_final:
	stmfd	sp!, {r4-r11, lr}
	mov	r0, r10
	bl	mod_final
	b	cm_return

@ r0 = error or 0 -> V set with r0, or V clear with r0 restored by caller
cm_return:
	cmp	r0, #0
	ldmfd	sp!, {r4-r11, lr}
	beq	1f
	SETV
	mov	pc, lr
1:	CLRV
	mov	pc, lr

@ SWI handler: r11 = SWI offset, r0-r9 = caller's registers
cm_swi:
	stmfd	sp!, {r0-r9, lr}
	mov	r0, r11
	mov	r1, sp
	bl	mod_swi
	cmp	r0, #0
	bne	1f
	ldmfd	sp!, {r0-r9, lr}
	CLRV
	mov	pc, lr
1:	add	sp, sp, #4			@ r0 = the error
	ldmfd	sp!, {r1-r9, lr}
	SETV
	mov	pc, lr

@ Service call handler: r1 = service number. r0-r9 may be changed by C.
cm_service:
	stmfd	sp!, {r0-r9, lr}
	mov	r0, sp
	bl	mod_service
	ldmfd	sp!, {r0-r9, pc}

@ *command N: r0 = command tail, r1 = number of parameters
	.macro	CM_COMMAND index
cm_command_\index:
	stmfd	sp!, {r4-r11, lr}
	mov	r2, r1
	mov	r1, r0
	mov	r0, #\index
	bl	mod_command
	b	cm_return
	.endm

cm_relocate_got:
	ldr	r0, 7f
1:	add	r0, r0, pc			@ r0 = module_start
	ldr	r1, 8f
2:	add	r1, r1, pc			@ r1 = &got_base
	ldr	r2, [r1]
	subs	r2, r0, r2			@ delta since the last relocation
	moveq	pc, lr
	str	r0, [r1]
	ldr	r1, 9f
3:	add	r1, r1, pc			@ r1 = __got_start
	ldr	r3, 10f
4:	add	r3, r3, pc			@ r3 = __got_end
5:	cmp	r1, r3				@ (r4 is free: cm_init saved it;
	ldrlo	r4, [r1]			@ r12 must survive for mod_init)
	addlo	r4, r4, r2
	strlo	r4, [r1], #4
	blo	5b
	mov	pc, lr
7:	.word	module_start - (1b + 8)
8:	.word	got_base - (2b + 8)
9:	.word	__got_start - (3b + 8)
10:	.word	__got_end - (4b + 8)

@ oserror *swix_regs(unsigned swi, regs *r): calls SWI (with the X bit),
@ r->r[0..9] in and out. Returns the error, or 0.
	.global	swix_regs
swix_regs:
	stmfd	sp!, {r1, r4-r11, lr}
	orr	r12, r0, #0x20000
	ldmia	r1, {r0-r9}
	swi	0x20071				@ XOS_CallASWIR12
	ldr	r12, [sp]
	stmia	r12, {r0-r9}
	movvc	r0, #0
	ldmfd	sp!, {r1, r4-r11, pc}

	.pushsection .data.got_base, "aw"
	.align	2
got_base:
	.word	0
	.popsection
