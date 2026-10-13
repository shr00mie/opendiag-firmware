> Imported reference from QuantexLab (ScanDoc developer docs).
> Captured for OpenDiag planning. Upstream may change; prefer the live URL for updates.
> Status on OpenDiag: see docs/at_commands.md and OpenDiag/specs/elm327/.
>
> **Not implemented on OpenDiag** (no Ethernet). See `specs/non-goals/hsfz.md`.
>
> Source: https://quantexlab.com/en/develop/elm327_hsfz.html

ELM327 BMW HSFZ Extension - Ethernet Diagnostics 
 English 
- Deutsch 
- Ελληνικά 
- English 
- Français 
- 日本語 
- Polski 
- Русский 
- 中文 Your region: Europe 
- Home Page 
- ScanDoc Development 
- ELM327 
# ELM327 BMW HSFZ

## Overview

ELM327 protocol extension for BMW HSFZ (High Speed Fahrzeug Zugang — high-speed vehicle access) support. Protocol E allows performing UDS diagnostics over Ethernet/TCP using the standard ELM327 AT interface. 
### How HSFZ works

```

Tester (ScanDoc)                    Vehicle (BMW)
┌──────────┐    TCP/IP     ┌─────────────┐    CAN/LIN    ┌─────┐
│ SA=F4    │───────────────│  BMW ZGW    │───────────────│ ECU │
│ (tester) │   Ethernet    │  (gateway)  │ internal bus  │(TA) │
└──────────┘   port 6801   └─────────────┘               └─────┘

```

HSFZ — a proprietary BMW protocol for diagnostics over Ethernet. Unlike the standard DoIP (ISO 13400), it uses its own frame format with a 6-byte header (4-byte length + 2-byte control code) and 1-byte addressing (instead
 of 2-byte as in DoIP). 

Three parameters must be configured: ``````````

| Parameter | Command | What it sets | How to find out |
| --- | --- | --- | --- |
| IP of ECU/Gateway | AT HI | Where to connect via TCP | AT HD (discovery) or set manually |
| Source Address | AT HS | Tester address (1 byte) | Default F4, usually does not need to be changed |
| Target Address | AT HT | Target ECU address (1 byte) | From the vehicle documentation |

If `HI` is not set (IP = `00000000`), `AT SP E` automatically performs discovery (UDP broadcast to port 6811), finds the first ECU and fills the parameters from its response: 
- `HI` ← IP address of the ECU/gateway (e.g. `A9FE0114` = 169.254.1.20) 
- `HT` ← logical address of the ECU (e.g. `10` — typically the ZGW) 
- `HS` ← remains `F4` (default) 

After that, a TCP connection to port 6801 is established → ready for UDS (Routing Activation is not required, unlike DoIP). 
### Differences from DoIP (protocol 'D')
````

|  | HSFZ (protocol 'E') | DoIP (protocol 'D') |
| --- | --- | --- |
| Standard | BMW proprietary | ISO 13400 |
| Addressing | 1 byte (00–FF) | 2 bytes (0000–FFFF) |
| TCP port | 6801 | 13400 |
| UDP port (discovery) | 6811 | 13400 |
| Routing Activation | Not required | Required |
| Frame header | 6 bytes (len[4] + ctrl[2]) | 8 bytes (ver[1] + inv[1] + type[2] + len[4]) |
| Tester address | F4 (1 byte) | 0E80 (2 bytes) |

## Examples

### Request to the gateway

`AT SP E` finds the ECU and connects on its own: 

```

>ATZ
ELM327 v2.3

>ATE0
OK

>AT SP E                 # discovery → HI=A9FE0114, HT=10, HS=F4
OK                       # TCP connection completed

>22 F1 90                # UDS Read VIN — request goes to the gateway (HT=10)
62 F1 90 57 42 41 50 48 35 43 35 35 42 41 31 32 33 34 35 36

```

### Request to a specific ECU behind the gateway

For example, the engine control unit: 

```

>ATZ
ELM327 v2.3

>ATE0
OK

>AT SP E                 # discovery + connection to the gateway
OK

>22 F1 90                # VIN request to the gateway (HT=10)
62 F1 90 57 42 41 50 48 35 43 35 35 42 41 31 32 33 34 35 36

>AT HT 12                # switch Target Address to the engine ECU
OK

>22 F1 90                # same request — now it goes to ECU 12 via the gateway
62 F1 90 ...

```

## HSFZ AT commands

### HI hhhhhhhh — ECU IP address

Sets the IPv4 address of the ECU (gateway) in hex format (4 bytes, big-endian). 

```

>AT HI A9FE0114
OK

```

Example: `A9FE0114` = 169.254.1.20 

Default value: `00000000` (not set). If the IP is not set when `AT SP E` is issued, automatic Vehicle Discovery (UDP broadcast to port 6811) is performed. 
### HS hh — Source Address

Sets the tester address (Source Address, SA) in hex format (1 byte). 

```

>AT HS F4
OK

```

Default value: `F4`. Usually does not need to be changed. 
### HT hh — Target Address

Sets the ECU address (Target Address, TA) in hex format (1 byte). 

```

>AT HT 10
OK

```

Default value: `00`. 
### HC hhhh — HSFZ TCP port

Sets the ECU TCP port in hex format. Must be set before `AT SP E`. Default is `1A91` (6801 decimal) — the standard BMW HSFZ port. Change it only if the ECU uses a non-standard port. 

```

>AT HC 1A91
OK

```

### HD — ECU discovery (Vehicle Discovery)

Performs UDP broadcast Vehicle Discovery to port 6811. Prints a list of found ECUs with their VIN, logical address, IP and MAC address. 

Automatically fills `HI` (IP) and `HT` (Target Address) from the first discovered ECU. 

When `HI=0`, `AT SP E` performs discovery on its own — a separate `AT HD` is not required. `AT HD` is useful when you need to see which ECUs are on the network before connecting. 

```

>AT HD
VIN:WBAPH5C55BA123456
ADDR:10
IP:A9FE0114
MAC:001A37B44210
OK

```

Multiple ECUs on the network: 

```

>AT HD
VIN:WBAPH5C55BA123456
ADDR:10
IP:A9FE0114
MAC:001A37B44210
VIN:WBAKF0C58CE123456
ADDR:20
IP:A9FE0115
MAC:001A37B44211
OK

```

If no ECUs are found: 

```

>AT HD
NO DATA

```

## Initialization sequence
````````````````````

| Scenario | Sequence |
| --- | --- |
| Automatic (gateway) | ATZ → AT SP E |
| ECU behind gateway (IP via discovery) | ATZ → AT HD → AT HT xx → AT SP E |
| ECU behind gateway (IP known) | ATZ → AT HI hhhhhhhh → AT HT xx → AT SP E |

### What happens on AT SP E

- Disconnect — disconnecting the current protocol 
- Connect — creating the HSFZ J2534 channel (HSFZ_PS = 0x8FFC) 
- Discovery (if IP is not set) — UDP broadcast to port 6811 to find the ECU 
- Set Config — setting IP, SA, TA, TCP port 
- TCP Connect — establishing the TCP connection to the ECU on port 6801 

Routing Activation is not required (unlike DoIP). 

If any step fails, `UNABLE TO CONNECT` is returned. 
## OBD data format

### Sending

Hex data is sent as the UDS payload. ELM327 automatically wraps it into an HSFZ Diagnostic Message with an SA+TA header (1-byte addressing). 

```

>22F190
62 F1 90 57 42 41 50 48 35 43 35 35 42 41 31 32 33 34 35 36

```

### Response (H0 — headers off, default)

Only the UDS payload, without the SA/TA header: 

```

62 F1 90 57 42 41 ...

```

### Response (H1 — headers on)

SA + TA (4 bytes in J2534 format) + UDS payload: 

```

10 00 F4 00 62 F1 90 57 42 41 ...

```

Header format: `[TA_hi=0] [TA_lo] [SA_hi=0] [SA_lo]` — address of the response sender (ECU → Tester). The high bytes are always `00`, since HSFZ uses 1-byte addressing. 
## Automatic protocol search

HSFZ is included in the automatic protocol search order (`AT SP 0`). On an HSFZ attempt: 
- A temporary HSFZ channel is created 
- Vehicle Discovery is performed (UDP broadcast to port 6811, 2 sec timeout) 
- If an ECU is found — connect via TCP 
- If the connection is successful — a probe OBD request is sent 
- If no ECU is found or there is no response — proceed to the next protocol 
## Parameter reset

The `AT D` (defaults) and `AT Z` (reset) commands reset all HSFZ parameters: 
- `HI` → `00000000`
- `HS` → `F4`
- `HT` → `00`
- `HC` → `1A91` (6801) 
## Limitations

- Only one ECU is supported at a time 
- Discovery finds up to 16 unique ECUs (deduplicated by logical address), but only one is connected to 
- TLS is not supported 
- Maximum UDS message size: 4095 bytes 
-  up 

Select a region to match content and online products to your location. 
Europe 
Russia 
Rest World
