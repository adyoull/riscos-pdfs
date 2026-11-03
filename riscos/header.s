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
	.word	pd_start - module_start		@ start: the filer task
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
help:	.asciz	"PDFS\t\t0.16.2 (3 Nov 2026)"
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

@ Service call handler: reject all but the ones that matter quickly:
@ Reset (&27), FSRedeclare (&40), StartWimp (&49), StartedWimp (&4A),
@ ResourceFSStarting (&60), DeviceDead (&79), USB (&D2).
pd_service:
	teq	r1, #0x40
	teqne	r1, #0x27
	teqne	r1, #0x49
	teqne	r1, #0x4A
	teqne	r1, #0x60
	teqne	r1, #0x79
	teqne	r1, #0xD2
	movne	pc, lr
	b	cm_service

@ Module start code, entered in USR mode by *Desktop_PDFSFiler or by
@ *RMRun (double-clicking the module): no stack is given, so it sets its
@ own (pd_task_stack in filer.c) before any C.
@
@ The start code can run while the filer task is already running: *RMRun
@ (a double-click) enters it after the module's callback has started the
@ filer. So it starts on a small stack of its own (pd_entry_stack), and
@ moves to the filer task's stack (pd_task_stack) only when it is to be the
@ filer (pd_run_filer). 0.09 started on the filer's stack, overwriting the
@ running filer's frames: "abort on instruction fetch" when it next ran.
pd_start:
	ldr	sp, 2f
1:	add	sp, sp, pc
	bl	filer_entry
	swi	0x11				@ OS_Exit (filer_entry doesn't return)
2:	.word	pd_entry_stack + 2048 - (1b + 8)

	.global	pd_run_filer
pd_run_filer:
	ldr	sp, 4f
3:	add	sp, sp, pc
	bl	filer_main
	swi	0x11				@ OS_Exit (filer_main doesn't return)
4:	.word	pd_task_stack + 16384 - (3b + 8)

@ The Free module's call for free space (Free_Register): r0-r11 as a
@ block for free_entry, which returns 1 to set Z (reason 3: same device).
@ Free calls it with the return address pushed on its stack, not in lr
@ (Push PC; LDR PC, entry), and has little stack to spare: so it runs on
@ a stack of its own (pd_free_stack, in filer.c) and returns with Pull PC.
	.global	pd_free_entry
pd_free_entry:
	str	sp, 3f				@ the caller's stack
	ldr	sp, 4f
5:	add	sp, sp, pc			@ ours
	stmfd	sp!, {r0-r11, lr}
	mov	r0, sp
	bl	free_entry
	cmp	r0, #1
	ldmfd	sp!, {r0-r11, lr}
	ldr	sp, 3f
	ldmfd	sp!, {pc}
3:	.word	0
4:	.word	pd_free_stack + 6144 - (5b + 8)

@ Callback (OS_AddCallBack) to start the filer when loaded in the desktop
	.global	pd_callback
pd_callback:
	stmfd	sp!, {r0-r12, lr}
	bl	filer_callback
	ldmfd	sp!, {r0-r12, pc}

@ ResourceFS's call to re-register our files (Service_ResourceFSStarting):
@ void call_resourcefs(void *code, void *ws, const void *files)
	.global	call_resourcefs
call_resourcefs:
	stmfd	sp!, {r4-r12, lr}
	mov	r12, r1
	mov	r3, r0
	mov	r0, r2
	mov	lr, pc
	mov	pc, r3
	ldmfd	sp!, {r4-r12, pc}

@ The ResourceFS block (tools/mksprites.py): Resources.PDFS.Sprites
	.global	resource_files
	.align	2
resource_files:
	.incbin	"out/obj/resources.bin"
	.align	2

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
	.word	0x00020000			@ 0 to 2 parameters
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

	.asciz	"PDFSEject"
	.align	2
	.word	cm_command_8 - module_start
	.word	0x00020001			@ 1 to 2 parameters
	.word	syn_eject - module_start
	.word	help_eject - module_start

	.asciz	"PDFSUSB"
	.align	2
	.word	cm_command_7 - module_start
	.word	0x00020000			@ 0 to 2 parameters
	.word	syn_usb - module_start
	.word	help_usb - module_start

	.asciz	"PDFSBackground"
	.align	2
	.word	cm_command_9 - module_start
	.word	0x00010000			@ 0 to 1 parameters
	.word	syn_bg - module_start
	.word	help_bg - module_start

	.asciz	"Desktop_PDFSFiler"
	.align	2
	.word	cm_command_6 - module_start
	.word	0x00000000
	.word	syn_desk - module_start
	.word	help_desk - module_start

	.word	0

help_bg:	.asciz	"*PDFSBackground sets how files are written to a device in the desktop. off (the default): as any filing system does, while the program writes them, so Filer_Action's window shows the whole copy. on: big new files are kept and sent afterwards by PDFS's filer, with a window of its own, so a program that saves a big file all at once isn't held up while it goes."
syn_bg:	.asciz	"Syntax: *PDFSBackground [on|off]"
help_eject:	.asciz	"*PDFSEject makes a storage safe to remove: it writes what's waiting and stops using it. With all, every storage of that device is ejected and its session is closed, so the device can be unplugged. Refused while files on it are open."
syn_eject:	.asciz	"Syntax: *PDFSEject <disc> [all]"
help_usb:	.asciz	"*PDFSUSB lists the connected USB devices' interfaces that have bulk endpoints, and which are MTP. *PDFSUSB control on|off lets PDFS use USB class requests (Cancel) when it recovers from errors: off by default, as some USB drivers do them synchronously."
syn_usb:	.asciz	"Syntax: *PDFSUSB [control on|off]"
help_desk:	.asciz	"*Desktop_PDFSFiler starts PDFS's icon bar filer. The desktop starts it; after loading PDFS in the desktop, use *WimpTask Desktop_PDFSFiler."
syn_desk:	.asciz	"Syntax: *Desktop_PDFSFiler"
help_ext:	.asciz	"*PDFSExtensions sets how names with a filetype extension are shown: hide (the default) shows Letter.txt as Letter, a Text file; show shows it as Letter/txt. Files saved from RISC OS always get the extension for their type, or ,xxx if it has none."
syn_ext:	.asciz	"Syntax: *PDFSExtensions [show|hide]"
help_pdfs:	.asciz	"*PDFS selects the Portable Device Filing System as the current filing system."
syn_pdfs:	.asciz	"Syntax: *PDFS"
help_mount:	.asciz	"*PDFSMount mounts portable devices: with no parameter, every MTP device connected by USB (the desktop does this by itself); USB<n> [<interface>] mounts that USB device (for ones that don't say they are MTP); <address>:<port> connects to a simulated device over TCP."
syn_mount:	.asciz	"Syntax: *PDFSMount [USB<n> [<interface>] | <address>:<port>]"
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
	CM_COMMAND 6
	CM_COMMAND 7
	CM_COMMAND 8
	CM_COMMAND 9
