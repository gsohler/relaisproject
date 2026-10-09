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
| `RELAY_RESTORE_AT_BOOT` | 1 = after power-up, pulse every relay into its last saved state; 0 = all off. |
| `RELAY_ACTIVE_LOW` | Set to 1 if the pulse must be a low level. |
| `XTAL_MHZ` | Crystal on the board: 4, 8, 12 or 16 (also `make XTAL_MHZ=…`). Look at the part next to pins PTG4/PTG5. |
| `USB_INTERNAL_VREG` | Keep at 1 unless the board supplies 3.3 V to VUSB33 externally. **If it does, set it to 0, otherwise the chip can be damaged.** |
| `USB_INTERNAL_PULLUP` | Keep at 1 unless there is an external 1.5 kΩ resistor on D+. |
| `USB_VID/PID` | 1209:0001 is the pid.codes *test* ID. Get your own free PID at https://pid.codes before you ship boards. |

## Build

```sh
sudo apt install sdcc make python3   # SDCC >= 4.0 with the s08 port
make                  # firmware file + bootloader
make test             # host tests: application and bootloader (gcc)
make XTAL_MHZ=16      # other crystal
make EXTRA_CFLAGS=-DRELAY_RESTORE_AT_BOOT=0  # override board.h settings
```

| Output | Use |
|---|---|
| `build/relay_fw-<version>.s19` | **Firmware file.** Copy it onto the `RELAYBOOT` drive. |
| `build/bootloader.s19` | Bootloader alone (USBDM). |
| `build/relay_combined-<version>.s19` | Bootloader + firmware in one file (`make combined`), for new boards. |
| `build/relay_cdc.s19` | Raw SDCC output of the application, standalone (without bootloader). |

The version comes from `FW_VERSION` in `src/main.c`. Increase it for every
release; `INFO.TXT` on the drive and the `ID` command show it.

`make test` runs the USB stacks, the command parser, the FAT emulation and
the S-record programming on the PC (gcc) against a model of the JM60 USB
engine and flash. If the uCsim simulator is installed
(`sudo apt install sdcc-ucsim`), it also runs the bootloader's disk code as
compiled by SDCC in the HCS08 simulator and compares the results with the
gcc build; this catches SDCC code generation and calling convention
problems. It does not replace a test on real hardware.

Note: the bootloader is compiled with `--stack-auto`. SDCC has no
stack-auto library for the s08, so the bootloader must not call library
functions (`memset`, `memcpy`, ...); `boot/util.h` has replacements and
`tools/checkmap.py --no-lib` fails the build if library code gets linked.

## Memory layout (`src/layout.h`)

| Range | Content |
|---|---|
| 0x1960-0xDBAF | application |
| 0xDBB0-0xDBBF | application info block (magic, CRC16, version), written last |
| 0xDBC0-0xDBFF | application vectors (hardware vector redirection) |
| 0xDC00-0xFFFF | bootloader, write-protected (NVPROT 0xDA, NVOPT 0x42) |
| 0xFFA0-0xFFA9 | serial number slot (8 bytes + CRC16), written at production |

The bootloader owns the reset vector and starts after every reset. If the
application is complete (info block present) and nobody asked for the
bootloader, it jumps to the application at once, before touching clock or
USB.

## Programming with USBDM (new boards)

```sh
make flash            # bootloader + firmware (mass erase first)
make flash-boot       # only the bootloader; then copy the firmware via USB
```

`FLASHPROG=<name>` selects another programmer executable. Mass erase also
removes the flash protection of the bootloader area.

The BDM connector is a 6-pin header (BKGD, GND, RESET, VDD).

## Firmware update over USB (bootloader)

1. Start the bootloader:
   `python3 tools/relayctl.py bootloader` (or type `BOOTLOADER` in a terminal).
   The board answers `OK`, disconnects and comes back as a USB drive
   **RELAYBOOT**. The relays keep their state while the bootloader runs
   (they are latching; the driver inputs are held at their inactive level).
2. Copy `build/relay_fw-<version>.s19` onto the drive.
   Desktop Linux mounts the drive with the `flush` option, so the data is
   written as soon as the copy finishes. If you mounted it by hand, run `sync`.
3. About one second after the last block, the board restarts with the new
   firmware (`/dev/ttyACM*` appears again). The relays go back to their saved
   state, as after every power-up.

Further details:

- `INFO.TXT` on the drive shows the bootloader version, the installed
  firmware version, the serial number and the result of the last update.
  Eject and reconnect the drive to see the current state; the operating
  system caches the directory.
- Ejecting the drive without an update starts the installed firmware.
- Without a complete firmware (new board, interrupted update, wrong or
  damaged file), the bootloader stays active after every reset. Just copy
  the file again.
- Emergency entry: bridge **SDA (white) to GND (yellow)** on the JST
  connector while plugging the board in.
- Only firmware files from `make firmware` are accepted. The raw
  `relay_cdc.s19` is rejected (`ERROR - wrong file`), because its vectors
  are in the bootloader area.
- How it works: the drive is an emulated FAT12 volume. All sectors written
  to the data area are scanned for S-records; FAT and directory writes are
  ignored. Each record is range-checked and programmed (the application
  area is erased on the first record). After the end record, the CRC16
  over the whole application area must match the info block, and only then
  is the info block programmed. A power failure during the update therefore
  leaves the bootloader active.

## Use

```sh
sudo cp tools/99-relayboard.rules /etc/udev/rules.d/ && sudo udevadm control --reload
python3 tools/relayctl.py on 3
python3 tools/relayctl.py get          # -> STATE 04
python3 tools/relayctl.py bootloader   # restart as USB drive for an update
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
| `BOOTLOADER` | `OK`, then restart as USB drive `RELAYBOOT` |
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

Relay state across power cycles: every `ON`, `OFF`, `TOGGLE` and `SET`
stores the new state in flash (`src/nvstate.c`). After power-up each relay
is pulsed into its saved state (the latching relays usually are in it
already; the pulse makes sure firmware and relays agree). The state lives in
the flash block 0x1200-0x15FF, outside the application area, so firmware
updates keep it; only a USBDM mass erase clears it (then all relays start
off). Two pages are used as a log: one byte pair per change, one page erase
per 255 changes, about 5 million changes in total. If the flash write fails,
the command answers `ERR state not saved` (the relay has switched anyway).

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
