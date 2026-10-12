# 6-Channel USB Relay Board

Open-source firmware and tools for a 6-channel relay board based on the
NXP/Freescale MC9S08JM60 (HCS08) with USB.

- 6 latching (bistable) relays: the state is kept without power
- one ACS712-20A Hall current sensor per channel (true RMS / 50 Hz RMS)
- USB CDC (virtual serial port) with simple text commands
- USB mass-storage bootloader: copy the firmware file onto the drive, done
- unique serial number per board
- optional ESP32-S3 WLAN gateway with a web page and HTTP API

> **Low voltage only.** The boards are not CE-certified. Use them only as a
> development board with safety extra-low voltage (max. 50 V AC / 120 V DC).
> Do not connect mains voltage.

## Repository layout

| Folder | Content |
|---|---|
| [`hcs08-relay-cdc/`](hcs08-relay-cdc/) | Board firmware, USB bootloader, production and host tools (SDCC) |
| [`relay-gateway/`](relay-gateway/) | ESP32-S3 WLAN gateway (ESP-IDF) |

## Quick start

Plug the board into USB. It shows up as a serial port (`/dev/ttyACM0` on
Linux, a COM port on Windows). Open it with any terminal program and type:

```
HELP
ID
ON 1
OFF ALL
STATE
CURRENT
```

Or use the Python helper:

```
python3 hcs08-relay-cdc/tools/relayctl.py on 3
```

## Firmware update

Send `BOOTLOADER` (or run `relayctl.py bootloader`). A USB drive
`RELAYBOOT` appears. Copy `relay_fw-<version>.s19` onto it; the board
restarts with the new firmware.

Building the firmware yourself, programming new boards with USBDM and all
technical details: see [`hcs08-relay-cdc/README.md`](hcs08-relay-cdc/README.md).

## License

MIT, see [LICENSE](LICENSE).
