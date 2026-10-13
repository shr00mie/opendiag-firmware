# OpenDIAG - Open Source OBD2 Interface (Firmware)

Firmware for the OpenDIAG OBD-II diagnostic adapter — an ESP32-S3 based interface that presents an
ELM327-compatible AT command interface over USB and Bluetooth LE, and talks to the physical buses
found on an OBD-II connector.

> [!WARNING]
> **No warranty. Use at your own risk.** This software is provided as-is, and
> everything it does to a vehicle is your responsibility. See
> [Disclaimer](#disclaimer).

## Documentation

Published docs: **[maximus64.github.io/opendiag-firmware](https://maximus64.github.io/opendiag-firmware/)**

In this tree:

- [docs/manual.md](docs/manual.md) — setup, modes, BLE, shell, troubleshooting
- [docs/at_commands.md](docs/at_commands.md) — ELM327 AT command support matrix
- [docs/ediabas.md](docs/ediabas.md) — Deep OBD / EdiabasLib DS2 mode
- [docs/kdcan.md](docs/kdcan.md) — K+DCAN transparent UART (BMWeb)

## Features

*   **ELM327 Compatible:** AT command set over USB and BLE for drop-in use with existing ELM327 OBD-II apps. Protocols 1–9 plus USER1/USER2 custom CAN (`AT PB`, `AT CEA`, Flow Control, etc.). Session-backed `AT PP`/`PPS` and CAN monitors `AT MA`/`MR`/`MT`.
*   **Multi-profile BLE:** Nordic UART plus Vgate / Veepeak / OBDLink-style GATT layouts so more phone apps find a write/notify pair. Separate OpenDIAG control service; `ble compat` and `ble name` for bring-up.
*   **Multiple link modes:** ELM327, SLCAN, J2534, Ediabas / Deep OBD (DS2), and K+DCAN UART. Switch with a short button tap, the debug-shell `mode` command, `AT VIF …`, or BLE `MODE=`. The selected mode stays sticky across disconnects (clears session state, does not force ELM327).
*   **OBD-II physical layers:** CAN (ISO 15765-4), K-Line and L-Line (ISO 9141-2, ISO 14230-4 KWP), SAE J1850 PWM and VPW. K-Line supports higher baud (including `AT IB115`) plus DS2 and raw framing used by Ediabas / K+DCAN.
*   **SLCAN (LAWICEL):** Raw CAN for `slcand`, SavvyCAN, python-can, and similar tools, without the ELM327 layer.
*   **J2534 pass-thru:** Windows driver path for diagnostic / reprogramming software (see the user manual).
*   **Pin control:** Programmable high-side and low-side drivers on the OBD connector, with a software-adjustable boost for manufacturer wake-up and programming voltages.
*   **Status LED:** Mode colour (cyan / green / magenta / white / blue) with solid vs breathing for USB vs BLE (or no holder); overlays for pairing and live pins.

## Building

Initialize the pinned upstream dependencies before firmware or host builds:

```bash
git submodule update --init --recursive
```

This is an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) project, developed against
IDF v6.1. Install and activate the toolchain first:

```bash
. $IDF_PATH/export.sh
```

Then build and flash:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM1 monitor   # the console is the SECOND port
```

The first port carries protocol traffic, not the console.

Run test suite:

```bash
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build
```

## Usage

Once flashed, the adapter enumerates as a USB device named **OpenDIAG** with two CDC ACM serial
ports, and advertises over BLE as **OpenDIAG** (or **OBDII** if you set `ble name OBDII`).

| | Purpose |
| --- | --- |
| USB CDC 0 | data link — ELM327 by default; also SLCAN, J2534, Ediabas, K+DCAN |
| USB CDC 1 | console and debug shell |
| BLE data characteristics | same data link over multi-profile GATT (Nordic / Vgate / Veepeak / OBDLink-style) |
| BLE control characteristic | control commands (`MODE=`, etc.) on the OpenDIAG control service |

The link starts in **ELM327**. When a client disconnects, session state is cleared but the
**selected mode stays put** until you change it (button tap, `mode`, `AT VIF …`, or `MODE=`).
After a power cycle it boots in ELM327 again.

Quick mode examples:

```text
mode slcan          # or: AT VIF SLCAN
mode j2534          # or: AT VIF PASSTHRU
mode ediabas        # Deep OBD / EdiabasLib DS2 — see docs/ediabas.md
mode kdcan          # BMWeb-style K-Line UART — see docs/kdcan.md
```

## Current status

Implemented and working:

- ELM327 protocols 1–9 (J1850 PWM/VPW, ISO 9141-2, ISO 14230-4 KWP 5-baud and fast init,
  ISO 15765-4 CAN 11/29-bit at 250k/500k)
- USER1 / USER2 custom CAN (protocols B/C): `AT PB`, CEA/CER, CRA/CF/CM, Flow Control — enough
  for dealer-style BMW CAN recipes; see `docs/at_commands.md`
- Programmable Parameters (`AT PP` / `PPS`) and CAN monitors (`AT MA` / `MR` / `MT`)
- ELM327 over USB CDC 0 and BLE (multi-profile GATT, strict or `ble compat`)
- SLCAN (LAWICEL) for `slcand` / ECU tools
- J2534 pass-thru front-end (Windows driver in releases)
- Ediabas / Deep OBD DS2 mode and K+DCAN transparent UART mode
- Sticky mode across disconnect; short-tap cycles modes; mode-coloured status LED
- Board control: high-side/low-side drivers, boost converter, voltage sense, calibration

Not yet implemented:

- SAE J1939 (protocol A) and its J1939-specific AT commands (`DM1`, `MP`, `JE`/`JS`, …)
- The second CAN transceiver
- Persisting the selected mode across a power cycle (sticky only lasts until reboot)
- DoIP / BMW HSFZ Ethernet (explicitly out of scope for this adapter)

`docs/at_commands.md` is the source of truth for which AT commands answer `OK` today.

## Disclaimer

This software is provided **as-is, without warranty of any kind**, express or
implied, including but not limited to warranties of merchantability and fitness
for a particular purpose, as set out in sections 15 and 16 of the
[GNU General Public License](LICENSE). **You use it entirely at your own risk.**

- **No liability.** In no event shall the authors or contributors be liable for
  any claim, damage, or other loss arising from the use of this software — a
  bricked ECU, an immobilised vehicle, diagnostic or repair costs, lost data,
  personal injury, or anything else — whether in contract, tort, or otherwise.
- **You are responsible for what you send to a vehicle.** Nothing here can tell
  whether an image belongs in the module you are writing it to. Verify the
  target, the file, and the addresses yourself.
- **Only work on vehicles you own or are authorised to modify.** Programming an
  ECU may void warranties, and doing it to someone else's vehicle without
  permission may be unlawful.
- **Check your local law.** Modifying emissions-related calibrations is
  regulated in many jurisdictions and may make a vehicle illegal to drive on
  public roads. Handling manufacturer firmware may also carry its own
  restrictions. This project takes no position on either — that is yours to
  establish.
- **Safety.** A module that stops responding mid-flash can leave a vehicle
  undriveable without warning. Work on a stationary vehicle, in a place where it
  can stay put, on a stable supply.

## License

OpenDIAG is free software: you may redistribute it and modify it under the terms
of **version 3 of the GNU General Public License**, as published by the Free
Software Foundation. No later version applies. The full text is in
[LICENSE](LICENSE).

The GPL is copyleft: anything you distribute that is derived from this code must
be released under the same licence, with source. It carries no warranty — see
sections 15 and 16 of the licence, and [Disclaimer](#disclaimer).
