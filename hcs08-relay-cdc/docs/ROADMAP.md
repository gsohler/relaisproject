# Roadmap

Planned after the relays work on real hardware (pin map found, RELAY_MAP_VALID = 1).

## 1. Bootloader (USB mass storage, drag & drop)

- Lives at the start of flash, ~6-8 KB, write-protected via NVPROT. The
  application is linked behind it and uses vector redirection.
- Enumerates as a USB drive (own PID). Copy a `.s19` file onto it: sectors
  containing S-records are parsed and programmed on the fly, everything else
  (directory entries, .DS_Store, ...) is ignored. S-record lines split across
  sector boundaries and out-of-order sectors must be handled (host test!).
- Erase application area on first S-record, verify CRC after S9, then start
  the application. Linux: eject / `sync` after copying.
- Shows `INFO.TXT` (bootloader version, firmware version, serial number).
- Entry:
  - CDC command `BOOTLOADER` (magic word in RAM + software reset)
  - automatically if the application CRC is invalid
  - emergency: SDA (white) bridged to GND (yellow) on the JST connector at power-up
- Relays are latching, so they keep their state while in the bootloader.
  The bootloader must never drive relay pins.

## 2. Serial number (stored in the bootloader area)

- Fixed address inside the protected bootloader flash, survives every
  application update. The JM60 has no unique ID, so the number is assigned.
- RANDOM, not sequential (no conclusions about production volume).
  64 bit, so collisions are negligible even for many thousands of boards
  (32 bit would give ~0.3 % collision chance at 5000 boards).
- Generated ONLY on the PC: the production script takes os.urandom(8),
  patches it into the bootloader S19 and programs it with USBDM. No
  on-device generation (silent failures would go unnoticed).
- The script keeps a log (CSV: serial, date, firmware version) and refuses
  a duplicate, so the PC side is checked too.
- Stored with a checksum; never rewritten. If the field is erased or the
  checksum is wrong, `SERIAL` answers `ERR no serial` and the USB serial
  string shows `NOSERIAL`, so a missing number is immediately visible.
- Used as USB iSerialNumber (stable /dev/serial/by-id names, Home Assistant),
  readable via CDC command `SERIAL` (and in `ID`), shown in `INFO.TXT`.
- Format: 16 hex digits, e.g. `SERIAL 3F9A07C2D41B88E5`.

## 3. Current measurement (ACS712ELCTR-20A per relay, 6 per board)

- ACS712 20 A: 5 V supply, 2.5 V at 0 A, 100 mV/A, ratiometric.
- To find out: MCU VDD (5 V or 3.3 V -> divider?), which ADC pin each sensor
  output (ACS712 pin 7, VIOUT) goes to (ADC inputs on PTB0-7, PTD0/1/3/4).
  Add an `ADC` debug command that prints all channels.
- Measure: sample several 50 Hz periods, subtract mean (offset), compute RMS.
  Commands `CURRENT` -> `I 1:0.42 2:0.00 ... 6:0.00` and a calibration command.
- Realistic lower limit ~0.1-0.2 A (sensor noise). Power only as I x 230 V.

## 4. WLAN via ESP32 (the JM60 has no USB host, so no WLAN dongle)

Option A - I2C on the JST connector (any ESP32, incl. ESP32-C3):
- JM60 firmware gets an I2C slave (PTC0/SCL, PTC1/SDA = blue/white wires)
  with the same command set as USB CDC.
- ESP32 does WLAN: ESPHome / MQTT / small web page -> Home Assistant.
- Check whether VDD on the connector can supply the ESP (~250 mA peaks at
  3.3 V); otherwise separate supply and only GND/SCL/SDA connected.
- Compatible with the SDA->GND bootloader emergency bridge (I2C idles high),
  but the ESP must not pull SDA low during board power-up.

Option B - ESP32-S2/S3 as USB host (no JM60 firmware change):
- ESP talks CDC-ACM to the board via its native USB-OTG port
  (ESP-IDF usb_host_cdc_acm; check ESPHome USB host support).
- ESP must supply 5 V on VBUS (feed 5 V into the ESP board's 5V/VBUS pin),
  OTG adapter cable needed.
