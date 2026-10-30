# PDFS: Portable Device Filing System
#
#   make              out/PDFS,ffa (the RISC OS module) and out/pdfs-host
#   make check        host tests against the simulator (tests/host_test.py)
#   make dist         out/PDFS-$(VERSION).zip, for RISC OS (filetypes kept)
#
# RCD_DIR: riscos-crossdev (default ../riscos-crossdev); RISCOS_TOOLCHAIN:
# its toolchain (default $(RCD_DIR)/toolchain/env). Both are set in
# riscos-crossdev's container image.
#
# Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.

RCD_DIR ?= $(abspath ../riscos-crossdev)
RISCOS_TOOLCHAIN ?= $(RCD_DIR)/toolchain/env
RELEASE_SUFFIX ?= -beta
VERSION := $(shell sed -n 's/^\#define PDFS_VERSION "\(.*\)"/\1/p' core/pdfs.h)$(RELEASE_SUFFIX)
T := $(RISCOS_TOOLCHAIN)/bin/arm-riscos-gnueabihf-
CC := $(T)gcc
AS := $(T)as
OBJCOPY := $(T)objcopy
NM := $(T)nm
OUT := out
OBJ := $(OUT)/obj

CFLAGS := -O2 -marm -march=armv7-a -mno-unaligned-access -fpie -fvisibility=hidden \
          -mgeneral-regs-only -ffreestanding -fno-common -fno-stack-protector \
          -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdinc \
          -isystem $(shell $(CC) -print-file-name=include) \
          -Wall -Wextra -Werror -Iriscos/include -Iriscos -Icore -std=gnu11
LDFLAGS := -nostdlib -static -Wl,--no-dynamic-linker -Wl,-T,riscos/module.ld \
           -Wl,--build-id=none -Wl,-z,norelro

CORE := core/fs.c core/mtp.c
OBJS := $(OBJ)/header.o $(OBJ)/module.o $(OBJ)/filer.o $(OBJ)/sendwin.o $(OBJ)/usb.o $(OBJ)/cmodule.o $(OBJ)/fs.o $(OBJ)/mtp.o

all: $(OUT)/PDFS,ffa $(OUT)/PDFS-usbemu,ffa $(OUT)/pdfs-host

# Test build: USB through tests/rol/usbemu.c (a fake USB phone bridged to
# the simulator), for tests/rol/run.sh --usbemu
EMU_OBJS := $(filter-out $(OBJ)/usb.o,$(OBJS)) $(OBJ)/usb-emu.o $(OBJ)/usbemu.o
$(OBJ)/usb-emu.o: riscos/usb.c riscos/usb.h riscos/cmodule.h core/pdfs.h core/transport.h | $(OBJ)
	$(CC) $(CFLAGS) -DUSB_EMU -c -o $@ $<
$(OBJ)/usbemu.o: tests/rol/usbemu.c riscos/cmodule.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OUT)/PDFS-usbemu,ffa: $(EMU_OBJS) riscos/module.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OBJ)/PDFS-usbemu.elf $(EMU_OBJS) -lgcc
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--defsym=__link_base=0x10000 -o $(OBJ)/PDFS-usbemu-64k.elf $(EMU_OBJS) -lgcc
	$(OBJCOPY) -O binary $(OBJ)/PDFS-usbemu.elf $(OBJ)/e0.bin
	$(OBJCOPY) -O binary $(OBJ)/PDFS-usbemu-64k.elf $(OBJ)/e1.bin
	python3 -B riscos/check-pic.py $(OBJ)/e0.bin $(OBJ)/e1.bin 0x10000 \
	  0x$$($(NM) $(OBJ)/PDFS-usbemu.elf | awk '$$3=="__got_start"{print $$1}') \
	  0x$$($(NM) $(OBJ)/PDFS-usbemu.elf | awk '$$3=="__got_end"{print $$1}')
	cp $(OBJ)/e0.bin $@

$(OBJ):
	mkdir -p $@

$(OBJ)/resources.bin: tools/mksprites.py | $(OBJ)
	python3 tools/mksprites.py $@
$(OBJ)/header.o: riscos/header.s riscos/cmodule.inc $(OBJ)/resources.bin | $(OBJ)
	$(AS) -I riscos -o $@ $<
$(OBJ)/usb.o: riscos/usb.c riscos/usb.h riscos/cmodule.h core/pdfs.h core/transport.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJ)/filer.o: riscos/filer.c riscos/filer.h riscos/sendwin.h riscos/cmodule.h core/pdfs.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJ)/sendwin.o: riscos/sendwin.c riscos/sendwin.h riscos/cmodule.h core/pdfs.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJ)/module.o: riscos/module.c riscos/usb.h riscos/filer.h riscos/cmodule.h core/pdfs.h core/plat.h core/transport.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJ)/cmodule.o: riscos/cmodule.c riscos/cmodule.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJ)/fs.o: core/fs.c core/*.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJ)/mtp.o: core/mtp.c core/*.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# Initialised pointers in data (tables of strings, say) need relocations
# that nothing applies: only the GOT is relocated. Refuse them.
$(OBJ)/PDFS.elf: $(OBJS) riscos/module.ld
	@for o in $(OBJS); do if $(T)readelf -S $$o | grep -q '\.rel\.data'; then \
	  echo "$$o has relocated data (pointers in an initialised table?): not allowed" >&2; exit 1; fi; done
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) -lgcc
$(OBJ)/PDFS-at64k.elf: $(OBJS) riscos/module.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--defsym=__link_base=0x10000 -o $@ $(OBJS) -lgcc

# Position-independent apart from the GOT (relocated at start): check by
# linking at two addresses and comparing.
$(OUT)/PDFS,ffa: $(OBJ)/PDFS.elf $(OBJ)/PDFS-at64k.elf riscos/check-pic.py
	$(OBJCOPY) -O binary $(OBJ)/PDFS.elf $(OBJ)/p0.bin
	$(OBJCOPY) -O binary $(OBJ)/PDFS-at64k.elf $(OBJ)/p1.bin
	python3 -B riscos/check-pic.py $(OBJ)/p0.bin $(OBJ)/p1.bin 0x10000 \
	  0x$$($(NM) $(OBJ)/PDFS.elf | awk '$$3=="__got_start"{print $$1}') \
	  0x$$($(NM) $(OBJ)/PDFS.elf | awk '$$3=="__got_end"{print $$1}')
	cp $(OBJ)/p0.bin $@

$(OUT)/pdfs-host: host/host.c $(CORE) core/*.h
	mkdir -p $(OUT)
	gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o $@ host/host.c $(CORE)

check: $(OUT)/pdfs-host
	python3 tests/host_test.py $(OUT)/pdfs-host

# The release zip: the module, the README, the licence and the Pi testing notes
dist: $(OUT)/PDFS,ffa
	rm -rf $(OUT)/dist && mkdir -p $(OUT)/dist/PDFS
	cp $(OUT)/PDFS,ffa $(OUT)/dist/PDFS/PDFS,ffa
	cp README.md CHANGELOG.md $(OUT)/dist/PDFS/
	cp LICENSE $(OUT)/dist/PDFS/LICENSE,fff
	cp docs/TESTING-ON-A-PI.md $(OUT)/dist/PDFS/Testing.md
	cp docs/REFERENCES.md $(OUT)/dist/PDFS/References.md
	rm -f $(OUT)/PDFS-$(VERSION).zip
	python3 $(RCD_DIR)/package/rozip.py $(OUT)/PDFS-$(VERSION).zip -C $(OUT)/dist PDFS
	@echo "made $(OUT)/PDFS-$(VERSION).zip"

clean:
	rm -rf $(OUT)

.PHONY: all check clean dist
