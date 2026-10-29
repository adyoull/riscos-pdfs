/* Link script for the test bed's C modules: one flat image, linked at 0.
   The header (.text.header) comes first. Code is compiled -fpie, so the
   only absolute addresses are in the GOT, which module.s relocates when
   the module starts (check-pic.sh checks that). .bss is kept inside the
   image (zero-filled), as a RISC OS module gets no separate bss. */
ENTRY(module_start)
SECTIONS
{
  . = DEFINED(__link_base) ? __link_base : 0;
  .text : {
    *(.text.header)
    *(.text .text.*)
    *(.rodata .rodata.* .rodata1)
  }
  .got : {
    __got_start = .;
    *(.got.plt) *(.igot.plt) *(.got) *(.igot)
    __got_end = .;
  }
  .data : {
    *(.data.got_base)
    *(.data .data.*)
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __module_end = .;
  }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
