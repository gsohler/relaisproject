# Roadmap

Planned after the relays work on real hardware (pin map found, RELAY_MAP_VALID = 1).

## 1. Bootloader (USB mass storage, drag & drop) - DONE (firmware 1.3, bootloader 1.0)

Implemented in `boot/`, see README "Firmware update over USB". Differences
from the original plan:
- Lives at the END of flash (0xDC00-0xFFFF), because the JM60 can only
  protect the top of flash and redirects the vectors to just below the
  protected area. Application 0x1960-0xDBFF.
- Same VID/PID as the application; the host tells them apart by the
  interface class (mass storage). The gateway refuses BOOTLOADER.
- Completeness check: info block with CRC16 written last (no separate
  "CRC fallback" needed: an incomplete update never gets an info block).
- Still to verify on real hardware: Windows and macOS hosts, update time.

## 1b. Bootloader 1.1 / 1.2 - DONE

- 1.1: NVOPT 0x3E (vector redirection on; 1.0 had 0x42 = off, so
  application interrupts crashed). Firmware 1.7+ checks the vector at runtime.
- 1.2: crash fallback (3 abnormal resets in a row -> stay in the
  bootloader), COP watchdog for firmware that declares it (info block flag),
  start reason in INFO.TXT. Boards with 1.0/1.1: `make reprogram SERIAL=...`.
- To verify on hardware: COP service sequence (0x55/0xAA to SRS) really
  keeps the COP quiet - a wrong sequence would show up as a reset every
  second and then the crash fallback.

## 2. Serial number - DONE (firmware 1.5, tools/production.py)

See README "Production: serial numbers". Random 64-bit number generated on
the PC, CRC16, slot 0xFFA0 in the protected bootloader area, CSV log with
duplicate check, USB check after programming. Shown as USB serial number,
in `ID`, `SERIAL` and `INFO.TXT`.

## 3. Current measurement - IMPLEMENTED (firmware 1.6), channels to verify

See README "Current measurement". Open:
- find the real ADC channels with `ADC` and enter them in board.h,
- check VDD (ADC VDD line) and the scale with a known load,
- noise floor on real hardware; maybe a small dead band,
- show the currents in the gateway (web page, /api/boards, /switch).

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
