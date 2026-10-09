# hcs08-relay-cdc

Firmware for a 6-channel relay board with an MC9S08JM60 (or JM32).
Each relay has an ON input and an OFF input. Switching means one 100 ms
pulse on the ON or OFF input, so the board uses 12 MCU pins.
The board appears as a USB virtual COM port (CDC-ACM). It works on Linux,
macOS and Windows 10/11 without a driver, and you control it with plain
text commands.

Everything builds on Linux with SDCC, without CodeWarrior. The USB stack
is small and self-contained (`src/usb_cdc.c`, about 600 lines, polled, no
interrupts).

## Pin mapping

Measured on the real board (pulses active high), entered in `src/board.h`:

| Relay | ON | OFF |
|---|---|---|
| 1 | PTE7 | PTE6 |
| 2 | PTE5 | PTE4 |
| 3 | PTF7 | PTF6 |
| 4 | PTF5 | PTF4 |
| 5 | PTF3 | PTF2 |
| 6 | PTF1 | PTF0 |

The debug commands `PULSE` and `SCAN`, used to find this mapping, were
removed in firmware 1.2. Version 1.1 still has them, if a different PCB
ever needs to be mapped.

## Before flashing: adapt `src/board.h`

| Setting | Meaning |
|---|---|
| `RELAY_PINS` | `{ ON pin, OFF pin }` per relay, written as `PIN(PE, 3)` for PTE3. |
| `RELAY_MAP_VALID` | 1 = mapping is valid. With 0 the firmware never drives a relay pin. |
| `RELAY_PULSE_MS` / `RELAY_GAP_MS` | Pulse length (100 ms) and pause between two pulses (20 ms). |
| `RELAY_OFF_AT_BOOT` | 1 = pulse all OFF inputs after power-up, so the firmware's state matches reality. |
| `RELAY_ACTIVE_LOW` | Set to 1 if the pulse must be a low level. |
| `XTAL_MHZ` | Crystal on the board: 4, 8, 12 or 16 (also `make XTAL_MHZ=…`). Look at the part next to pins PTG4/PTG5. |
| `USB_INTERNAL_VREG` | Keep at 1 unless the board supplies 3.3 V to VUSB33 externally. **If it does, set it to 0, otherwise the chip can be damaged.** |
| `USB_INTERNAL_PULLUP` | Keep at 1 unless there is an external 1.5 kΩ resistor on D+. |
| `USB_VID/PID` | 1209:0001 is the pid.codes *test* ID. Get your own free PID at https://pid.codes before you ship boards. |

## Build

```sh
sudo apt install sdcc make      # SDCC >= 4.0 with the s08 port
make                            # -> build/relay_cdc.s19
make XTAL_MHZ=16                # other crystal
make CODE_LOC=0x8000            # MC9S08JM32 (flash starts at 0x8000)
make test                       # host-side unit test of USB stack + parser (gcc)
make EXTRA_CFLAGS=-DRELAY_OFF_AT_BOOT=0  # override board.h settings
```

Current size: about 5 KB flash and 0.5 KB RAM.

`make test` runs the USB stack and the command parser on the PC against
a model of the JM60 buffer descriptors. It covers enumeration, SET_ADDRESS
timing, the descriptor chain, ZLP handling, STALL recovery, endpoint halt,
line coding, the relay commands and the exact pulse sequence on each pin.
It does not replace a test on real
hardware.

## Flash

Use a BDM programmer, for example **USBDM** (open source, runs on Linux,
cheap hardware such as the "USBDM JS16" clones). Use the HCS08 flash
programmer, select the target **MC9S08JM60**, load `build/relay_cdc.s19`
and program it. The image already contains NVOPT = 0x42 (unsecured) and
NVPROT = 0xFF.

The BDM connector is a 6-pin header (BKGD, GND, RESET, VDD). On most
JM60 boards it is next to the MCU.

## Use

```sh
sudo cp tools/99-relayboard.rules /etc/udev/rules.d/ && sudo udevadm control --reload
python3 tools/relayctl.py on 3
python3 tools/relayctl.py get          # -> STATE 04
```

Or use a terminal program (`picocom /dev/relayboard`). Baud rate settings
are ignored.

| Command | Reply |
|---|---|
| `ON <n\|ALL>` – pulse the ON input(s), always | `OK` |
| `OFF <n\|ALL>` – pulse the OFF input(s), always | `OK` |
| `TOGGLE <n>` | `OK` |
| `SET <hex>` – all relays at once from a bit mask | `OK` |
| `GET` | `STATE hh` |
| `ID` | `ID <product> FW <version>` |
| `HELP` or `?` | command list |
| anything invalid | `ERR <reason>`; an unknown command is echoed as `ERR command '<line>'` |

`SET` takes a hex bit mask where bit 0 is relay 1 and bit 5 is relay 6
(00..3F). `SET 25` = binary 10 0101 turns relays 1, 3 and 6 on and 2, 4
and 5 off. Only relays whose state changes get a pulse. `GET` returns the
same format.

The firmware only knows the state it switched to. It cannot read back the
actual relay state. Pulses run one after another, so `ON ALL` takes about
0.7 s. The reply comes after the last pulse.

Commands are case-insensitive and end with CR and/or LF. Replies are sent
only while the host has the port open (DTR set). pyserial, picocom and
Home Assistant all set DTR automatically.

With `RELAY_OFF_AT_BOOT 1`, all relays are switched off after power-up.
They keep their state when the host closes the port or re-enumerates.

## Implementation notes

- Clock: PEE mode, crystal → PLL → 48 MHz, 24 MHz bus (as in NXP AN3560).
- USB RAM 0x1860–0x195F: BDT at 0x1860 (order EP0 IN, EP0 OUT, EP1…), then
  buffers EP0 OUT/IN 32 bytes each, EP1 16, EP2 64, EP3 64.
- EP0 state machine follows the Microchip/Freescale pattern: the SIE sets
  TSUSPEND on SETUP and firmware clears it. During an IN data stage, EP0
  OUT is armed without DTS so that both the status ZLP and an early SETUP
  are accepted. The new address from SET_ADDRESS is applied after the
  status stage.
- CDC interface protocol is 0 (no AT commands). The udev rule also keeps
  ModemManager away from the port.
- 1 ms time base: TPM1 overflow flag, polled. While waiting during a pulse,
  USB keeps being serviced.
- The COP watchdog is switched off in `_sdcc_external_startup()`, before
  RAM initialisation.

## Home Assistant

A simple setup uses `command_line` switches that call `relayctl.py`. For
something more integrated, a small custom integration or an ESPHome-style
serial bridge can talk to the port directly.
