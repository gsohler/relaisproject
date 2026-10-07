# MC9S08JM60 USB CDC relay firmware - SDCC build
#
#   make                 build build/relay_cdc.s19
#   make XTAL_MHZ=16     other crystal (4, 8, 12, 16)
#   make clean

TARGET   := relay_cdc
BUILD    := build
SRCS     := src/main.c src/usb_cdc.c src/usb_desc.c src/relay.c src/timer.c

CC       := sdcc
XTAL_MHZ ?= 12

# MC9S08JM60 memory map:
#   direct page RAM 0x00B0-0x00FF, RAM up to 0x10AF (4 KiB)
#   USB RAM 0x1860-0x195F (used via fixed addresses, not by the linker)
#   flash 0x1960-0xFFAF (+0x10B0-0x17FF, unused here), NV regs 0xFFB0-0xFFBF,
#   vectors 0xFFC0-0xFFFF
# For the MC9S08JM32 flash starts at 0x8000: make CODE_LOC=0x8000
CODE_LOC ?= 0x1960

# Memory options must be given at compile time too: the startup code
# (generated into main.rel) loads SP from --stack-loc.
MEMFLAGS := --code-loc $(CODE_LOC) \
            --data-loc 0x00B0 \
            --xram-loc 0x0100 --xram-size 0x0F00 \
            --stack-loc 0x10AF
CFLAGS  := -ms08 --std-sdcc11 --opt-code-size -DXTAL_MHZ=$(XTAL_MHZ) -Isrc $(MEMFLAGS) $(EXTRA_CFLAGS)
LDFLAGS := -ms08 --out-fmt-s19 $(MEMFLAGS)

OBJS := $(patsubst src/%.c,$(BUILD)/%.rel,$(SRCS))

all: $(BUILD)/$(TARGET).s19

$(BUILD)/%.rel: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).s19: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $(BUILD)/$(TARGET).s19
	@echo "Built $@"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean

# Host-side unit test of the USB stack and command parser (needs gcc)
test:
	mkdir -p $(BUILD)
	gcc -DHOST_TEST -DRELAY_MAP_VALID=1 -DXTAL_MHZ=12 -Isrc -Wall -Wno-main -g -fsanitize=address,undefined \
	    -o $(BUILD)/host_test test/host_test.c src/usb_cdc.c src/usb_desc.c src/relay.c src/timer.c
	./$(BUILD)/host_test

.PHONY: test

# Program the board with USBDM (board powered by its own USB -> no -vdd).
# Find the executable name with: dpkg -L usbdm | grep -i programmer
FLASHPROG ?= UsbdmFlashProgrammer
flash: $(BUILD)/$(TARGET).s19
	$(FLASHPROG) -target=HCS08 -device=MC9S08JM60 -useReset \
	    -masserase -unsecure -program -execute $(BUILD)/$(TARGET).s19

.PHONY: flash
