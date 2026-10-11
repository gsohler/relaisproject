# relay-gateway

WLAN gateway for the HCS08 6-relay boards on an **ESP32-S3** (intended board:
N16R8 dev board with two USB-C ports). The relay boards are plugged into the
ESP's USB-OTG port, directly or through a powered USB hub. The gateway serves
a web page and a small JSON API.

```
[browser / Home Assistant] --WLAN--> [ESP32-S3] --USB-OTG--> [powered hub] --> relay boards
```

## Limits

The ESP32-S3 USB host controller has **8 channels**. Every enumerated USB device
keeps one permanently (its control pipe), a hub keeps two, and the board that
is being talked to needs two more for its bulk endpoints. Therefore boards are
never kept open. Each command opens the board, sends one line, reads one reply
and closes it again. That leaves:

- **at most about 4 relay boards behind one hub**
- more boards: use a second gateway (each one has its own name in the network)

## Linux setup (one time)

```sh
# 1. Packages (Debian/Ubuntu)
sudo apt install git wget flex bison gperf python3 python3-pip python3-venv \
     cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# 2. ESP-IDF v5.5 (needs ~4 GB)
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3

# 3. Access to serial ports without root, then log out and in again
sudo usermod -aG dialout $USER

# 4. In every new shell (or put an alias into ~/.bashrc):
. ~/esp/esp-idf/export.sh
#    alias get_idf='. $HOME/esp/esp-idf/export.sh'
```

## Build and flash

```sh
cd relay-gateway
idf.py build                       # first build fetches the mdns component
idf.py -p /dev/ttyACM0 flash monitor
```

- Use the port labelled **UART** or **COM** for flashing and logs. Depending on
  the USB-serial chip it appears as `/dev/ttyACM0` (CH343) or `/dev/ttyUSB0`
  (CP2102). `ls /dev/tty{ACM,USB}*` before and after plugging in shows which.
- Leave the monitor with `Ctrl+]`.
- If flashing does not start: hold **BOOT**, press **RST**, release BOOT.
- Settings (web password, AP password, poll interval): `idf.py menuconfig`
  → *Relay Gateway*.

## Hardware

- **USB-OTG port** = the port labelled **USB** (GPIO19/20). Connect the hub
  or a relay board there, using a USB-C to USB-A OTG adapter.
- **5 V on the OTG port:** the dev board does not output 5 V on this port by
  default. Use a **powered hub**. Many dev boards have a solder bridge marked
  **USB-OTG** (on the back, near the ports). Close it if your hub only works
  when the host supplies 5 V. The gateway is then powered via the UART port
  or the 5V pin.
- The relay boards are powered from the hub.

## First start (from a phone)

1. Without a stored WLAN, the gateway opens the access point **RelayGW-XXXX**
   (password **relaygateway**, changeable in menuconfig).
2. Join it with your phone. The setup page opens **by itself**, like a hotel
   WLAN login (captive portal: DNS answers every name with 192.168.4.1 and
   phones' connectivity checks are redirected to `/setup`). If not, open
   **http://192.168.4.1** manually.
3. Tap your network and enter the password. The gateway **tests the
   connection first** while the access point stays up, and then shows the new
   address, for example `http://192.168.178.57/` and
   `http://relaygw-xxxx.local/`. A wrong password or an unreachable network is
   reported on the same page.
4. Switch your phone back to your home WLAN and open the address. Use
   *Add to home screen* in the browser menu to start it like an app. iOS opens
   it full screen. On Android it is a shortcut, because full app mode would
   need HTTPS.
5. The setup access point closes two minutes after success. If the home WLAN
   becomes unreachable, it opens again automatically. You can also change the
   WLAN later via *WLAN einrichten* on the main page.

**Security:** By default, anyone in the WLAN can switch the relays. Set a
password in menuconfig (*Web interface password*, user `admin`), especially
when 230 V loads are connected.

## Languages

The web pages are available in German, English and French. The language is
chosen from the browser language and can be switched at the top right. The
choice is remembered in the browser. Texts are in `main/www/i18n.js`.

## Relay names

Each relay can get a name: the **Names** button below a board. Names are
stored on the gateway (NVS) per *board serial number + relay number*. They
survive restarts and reflashing, are shown on the buttons and can be used to
switch over HTTP. Names are unique (not case-sensitive) and 1-32 characters
long.

Names need a unique serial number per board (board firmware 1.5 or later,
programmed with `make production`). Boards without a number all report
`NOSERIAL`: they share their names, and `/switch` answers
`ERR serial number not unique` if more than one of them is connected.

## Simple HTTP switching

Plain-text answers, made for `curl`, scripts and Home Assistant
(`rest_command`):

```sh
curl "http://relaygw-xxxx.local/switch?name=Pumpe&state=on"       # -> OK
curl "http://relaygw-xxxx.local/switch?name=Pumpe&state=toggle"   # -> OK
curl "http://relaygw-xxxx.local/switch?name=Pumpe"                # -> on / off
curl "http://relaygw-xxxx.local/switch?serial=3F9A07C2D41B88E5&relay=3&state=off"
```

`state` is `on`, `off`, `toggle` (or `1`/`0`). Names with spaces or umlauts
must be URL-encoded (`K%C3%BCche`, `+` for a space); curl does that with
`--get --data-urlencode "name=Küche" --data "state=on"`.
Errors answer `ERR <reason>` with an HTTP error status (404 unknown name,
503 board not connected, ...). With password: `curl -u admin:PASSWORD …`

## API

```sh
curl http://relaygw-xxxx.local/api/boards
curl -X POST http://relaygw-xxxx.local/api/cmd -d '{"addr":3,"cmd":"ON 2"}'
```

`addr` is the USB address from `/api/boards`. Commands are the same as over
USB: `ON n|ALL`, `OFF n|ALL`, `TOGGLE n`, `SET hh`, `GET`, `ID`.
`BOOTLOADER` is refused: the board would turn into a USB drive that the
gateway cannot use (firmware updates over a direct USB connection). With password: `curl -u admin:PASSWORD …`

## Notes on the implementation

- `main/usb_boards.c`: discovery (VID/PID 1209:0001, also behind hubs),
  per-command sessions, round-robin status poll (`GET`) every 2 s per board.
  At the start of each session the board's bulk endpoints are reset with
  CLEAR_FEATURE(ENDPOINT_HALT), because new host pipes start with DATA0. Then
  DTR is set and stale output is discarded.
- Boards are identified by their USB serial number (random, set at
  production). Boards without one report `NOSERIAL` and are told apart by
  USB address and hub port (`location`).
- Planned: current values (`CURRENT`) per relay as soon as the board firmware
  supports it, MQTT / Home Assistant auto-discovery, OTA updates.
