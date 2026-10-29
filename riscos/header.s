@ PDFS (Portable Device Filing System): module header, filing system
@ information block and the veneers from FileSwitch's entry points to C.
@
@ Each FSEntry veneer saves R0-R7 on the stack as a pd_regs block, calls
@ pdfs_fs_entry(entry, regs), and returns R0-R7 from the block (V clear),
@ or the error in R0 with V set. R8-R11 are preserved by the C code.
@
@ Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.

	.syntax	unified
	.arm
	.section .text.header, "ax"
	.global	module_start
module_start:
	.word	0				@ start
	.word	cm_init - module_start
	.word	cm_final - module_start
	.word	pd_service - module_start
	.word	title - module_start
	.word	help - module_start
	.word	commands - module_start
	.word	0				@ no SWIs yet (chunk not allocated)
	.word	0
	.word	0
	.word	0
	.word	0				@ messages file
	.word	flags - module_start

title:	.asciz	"PDFS"
help:	.asciz	"PDFS\t\t0.01 (29 Oct 2026) test build"
	.align	2
flags:	.word	1				@ 32-bit compatible

@ Filing system information block (offsets from module_start)
	.global	fs_info_block
fs_info_block:
	.word	fs_name - module_start
	.word	fs_startup - module_start
	.word	fs_open - module_start
	.word	fs_getbytes - module_start
	.word	fs_putbytes - module_start
	.word	fs_args - module_start
	.word	fs_close - module_start
	.word	fs_file - module_start
	@ information word: MultiFS extensions (23), FileSwitch does load and
	@ save with open/GBPB/close (20, 19), extra info word present (17),
	@ no limit on open files, filing system number PDFS_FSNUM (&FA)
	.word	(1 << 23) | (1 << 20) | (1 << 19) | (1 << 17) | 0xFA
	.word	fs_func - module_start
	.word	0				@ no unbuffered GBPB
	.word	(1 << 5)			@ extra: no CatalogObjects (FileSwitch makes them)

fs_name:	.asciz	"PDFS"
fs_startup:	.asciz	"Portable Device Filing System"
	.align	2

	.macro	FSENTRY name, index
\name:
	stmfd	sp!, {r0-r7, lr}
	mov	r0, #\index
	mov	r1, sp
	bl	pdfs_fs_entry
	b	fs_return
	.endm

	FSENTRY	fs_open, 0
	FSENTRY	fs_getbytes, 1
	FSENTRY	fs_putbytes, 2
	FSENTRY	fs_args, 3
	FSENTRY	fs_close, 4
	FSENTRY	fs_file, 5
	FSENTRY	fs_func, 6

fs_return:
	cmp	r0, #0
	bne	1f
	ldmfd	sp!, {r0-r7, lr}
	msr	APSR_nzcvq, #0			@ V clear
	mov	pc, lr
1:	add	sp, sp, #4			@ r0 = the error
	ldmfd	sp!, {r1-r7, lr}
	msr	APSR_nzcvq, #0x10000000		@ V set
	mov	pc, lr

@ Service call handler: only Service_FSRedeclare (&40) matters, so
@ reject everything else quickly before going to C.
pd_service:
	teq	r1, #0x40
	movne	pc, lr
	b	cm_service

@ *commands
commands:
	.asciz	"PDFS"
	.align	2
	.word	cm_command_0 - module_start
	.word	0x00000000			@ no parameters
	.word	syn_pdfs - module_start
	.word	help_pdfs - module_start

	.asciz	"PDFSMount"
	.align	2
	.word	cm_command_1 - module_start
	.word	0x00020001			@ 1 to 2 parameters
	.word	syn_mount - module_start
	.word	help_mount - module_start

	.asciz	"PDFSDismount"
	.align	2
	.word	cm_command_2 - module_start
	.word	0x00010000			@ 0 to 1 parameters
	.word	syn_dismount - module_start
	.word	help_dismount - module_start

	.asciz	"PDFSDevices"
	.align	2
	.word	cm_command_3 - module_start
	.word	0x00000000
	.word	syn_devices - module_start
	.word	help_devices - module_start

	.asciz	"PDFSLog"
	.align	2
	.word	cm_command_4 - module_start
	.word	0x00010000			@ 0 to 1 parameters
	.word	syn_log - module_start
	.word	help_log - module_start

	.asciz	"PDFSExtensions"
	.align	2
	.word	cm_command_5 - module_start
	.word	0x00010000			@ 0 to 1 parameters
	.word	syn_ext - module_start
	.word	help_ext - module_start

	.word	0

help_ext:	.asciz	"*PDFSExtensions sets how names with a filetype extension are shown: hide (the default) shows Letter.txt as Letter, a Text file; show shows it as Letter/txt. Files saved from RISC OS always get the extension for their type, or ,xxx if it has none."
syn_ext:	.asciz	"Syntax: *PDFSExtensions [show|hide]"
help_pdfs:	.asciz	"*PDFS selects the Portable Device Filing System as the current filing system."
syn_pdfs:	.asciz	"Syntax: *PDFS"
help_mount:	.asciz	"*PDFSMount connects to a simulated portable device over TCP (for testing)."
syn_mount:	.asciz	"Syntax: *PDFSMount <address>:<port>"
help_dismount:	.asciz	"*PDFSDismount disconnects a portable device (all devices if no disc is given)."
syn_dismount:	.asciz	"Syntax: *PDFSDismount [<disc>]"
help_devices:	.asciz	"*PDFSDevices lists the portable devices connected and their storages."
syn_devices:	.asciz	"Syntax: *PDFSDevices"
help_log:	.asciz	"*PDFSLog shows PDFS's recent log messages. *PDFSLog on|off turns logging on or off; *PDFSLog trace also logs every FileSwitch call."
syn_log:	.asciz	"Syntax: *PDFSLog [on|off|trace]"
	.align	2

	.include "cmodule.inc"

	CM_COMMAND 0
	CM_COMMAND 1
	CM_COMMAND 2
	CM_COMMAND 3
	CM_COMMAND 4
	CM_COMMAND 5
