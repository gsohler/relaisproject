# MC9S08JM60 relay board - SDCC build
#
#   make                build everything (firmware file + bootloader)
#   make firmware       build/relay_fw-<version>.s19  -> copy onto the RELAYBOOT drive
#   make boot           build/bootloader.s19
#   make combined       build/relay_combined-<version>.s19 (bootloader + firmware)
#   make flash          program bootloader + firmware with USBDM (new boards)
#   make flash-boot     program only the bootloader with USBDM
#   make production     new boards: bootloader + firmware + random serial number
#   make test           host tests (gcc) + bootloader code in the HCS08 simulator
#   make clean
#
#   make XTAL_MHZ=16    other crystal (4, 8, 12, 16)
#
# Updating a board without USBDM:
#   python3 tools/relayctl.py bootloader      (or send BOOTLOADER in a terminal)
#   -> a USB drive RELAYBOOT appears; copy build/relay_fw-<version>.s19 onto it
#   -> the board restarts with the new firmware

BUILD    := build
CC       := sdcc
XTAL_MHZ ?= 12
PYTHON   ?= python3

VERSION  := $(shell sed -n 's/^\#define FW_VERSION "\(.*\)"/\1/p' src/main.c)

APP_SRCS  := src/main.c src/usb_cdc.c src/usb_desc.c src/relay.c src/timer.c src/flash.c src/nvstate.c src/serial.c src/current.c src/nvfix.c
BOOT_SRCS := boot/boot_main.c boot/usb_msc.c boot/disk.c boot/decide.c src/flash.c boot/util.c

APP_RAW   := $(BUILD)/relay_cdc.s19
FIRMWARE  := $(BUILD)/relay_fw-$(VERSION).s19
BOOTLOADER:= $(BUILD)/bootloader.s19
COMBINED  := $(BUILD)/relay_combined-$(VERSION).s19

# MC9S08JM60 memory map (see src/layout.h):
#   direct page RAM 0x00B0-0x00FF, RAM up to 0x10AF (4 KiB),
#   0x10AA-0x10AF crash counter + bootloader request word -> stacks start at 0x10A9
#   USB RAM 0x1860-0x195F (used via fixed addresses, not by the linker)
#   application 0x1960-0xDBFF, bootloader 0xDC00-0xFFFF
# Memory options must be given at compile time too: the startup code
# (generated into the module with main()) loads SP from --stack-loc.
RAMFLAGS  := --data-loc 0x00B0 --xram-loc 0x0100 --xram-size 0x0F00 --stack-loc 0x10A9
APP_MEM   := --code-loc 0x1960 $(RAMFLAGS)
BOOT_MEM  := --code-loc 0xDC00 $(RAMFLAGS)
BOOT_OPT  := --stack-auto

COMMON    := -ms08 --std-sdcc11 --opt-code-size -DXTAL_MHZ=$(XTAL_MHZ) -Isrc $(EXTRA_CFLAGS)

APP_OBJS  := $(patsubst src/%.c,$(BUILD)/app/%.rel,$(APP_SRCS))
BOOT_OBJS := $(patsubst %.c,$(BUILD)/boot/%.rel,$(notdir $(BOOT_SRCS)))

all: firmware boot

firmware: $(FIRMWARE)
boot: $(BOOTLOADER)
combined: $(COMBINED)

# ---- application ----
$(BUILD)/app/%.rel: src/%.c src/*.h | $(BUILD)/app
	$(CC) $(COMMON) $(APP_MEM) -c $< -o $@

$(APP_RAW): $(APP_OBJS)
	$(CC) -ms08 --out-fmt-s19 $(APP_MEM) $(APP_OBJS) -o $(BUILD)/app/relay_cdc.s19
	$(PYTHON) tools/checkmap.py $(BUILD)/app/relay_cdc.map --code-end 0xDBB0
	cp $(BUILD)/app/relay_cdc.s19 $@

$(FIRMWARE): $(APP_RAW) tools/mkfw.py
	$(PYTHON) tools/mkfw.py $(APP_RAW) --version $(VERSION) -o $@

# ---- bootloader ----
vpath %.c boot src
$(BUILD)/boot/%.rel: %.c boot/*.h src/*.h | $(BUILD)/boot
	$(CC) $(COMMON) $(BOOT_OPT) -Iboot $(BOOT_MEM) -c $< -o $@

$(BOOTLOADER): $(BOOT_OBJS)
	$(CC) -ms08 --out-fmt-s19 $(BOOT_MEM) $(BOOT_OBJS) -o $(BUILD)/boot/bootloader.s19
	$(PYTHON) tools/checkmap.py $(BUILD)/boot/bootloader.map --code-end 0xFFA0 --no-lib
	cp $(BUILD)/boot/bootloader.s19 $@

$(COMBINED): $(BOOTLOADER) $(FIRMWARE)
	$(PYTHON) tools/mkfw.py --combine $(BOOTLOADER) $(FIRMWARE) -o $@

$(BUILD)/app $(BUILD)/boot:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

# ---- host tests (gcc) ----
HOSTCC := gcc -DHOST_TEST -DXTAL_MHZ=12 -Isrc -Wall -Wno-main -g -fsanitize=address,undefined

test: $(FIRMWARE)
	mkdir -p $(BUILD)
	$(HOSTCC) -DRELAY_MAP_VALID=1 -o $(BUILD)/host_test \
	    test/host_test.c src/usb_cdc.c src/usb_desc.c src/relay.c src/timer.c src/flash.c src/nvstate.c src/serial.c src/current.c src/nvfix.c -lm
	./$(BUILD)/host_test
	$(HOSTCC) -Iboot -o $(BUILD)/boot_test \
	    test/boot_test.c boot/usb_msc.c boot/disk.c boot/decide.c src/flash.c boot/util.c
	./$(BUILD)/boot_test $(FIRMWARE) $(APP_RAW)
	@if command -v shc08 >/dev/null; then $(PYTHON) test/sim/run_sim.py $(FIRMWARE); \
	 else echo "simulator test skipped (sudo apt install sdcc-ucsim)"; fi

# ---- programming with USBDM ----
# Board powered by its own USB. Mass erase removes the flash protection of
# the bootloader area as well.
# -security=image: program NVPROT/NVOPT exactly as in the image. Do NOT use
# -unsecure: it replaces them with default values (no flash protection,
# FNORED = 1 -> no vector redirection -> CURRENT has no interrupt).
FLASHPROG ?= UsbdmFlashProgrammer
USBDM     := $(FLASHPROG) -target=HCS08 -device=MC9S08JM60 -useReset -masserase -security=image -program -execute

flash: $(COMBINED)
	$(USBDM) $(COMBINED)

flash-boot: $(BOOTLOADER)
	$(USBDM) $(BOOTLOADER)

# Production: random serial number per board, programmed with USBDM, logged
production: $(COMBINED)
	$(PYTHON) tools/production.py --image $(COMBINED) --flashprog "$(FLASHPROG)"

# Reprogram a board (e.g. new bootloader) and keep its serial number:
#   make reprogram SERIAL=3F9A07C2D41B88E5
reprogram: $(COMBINED)
	$(PYTHON) tools/production.py --image $(COMBINED) --flashprog "$(FLASHPROG)" --serial $(SERIAL)

.PHONY: all firmware boot combined clean test flash flash-boot production reprogram
