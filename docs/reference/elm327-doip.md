> Imported reference from QuantexLab (ScanDoc developer docs).
> Captured for OpenDiag planning. Upstream may change; prefer the live URL for updates.
> Status on OpenDiag: see docs/at_commands.md and OpenDiag/specs/elm327/.
>
> **Not implemented on OpenDiag** (no Ethernet). See `specs/non-goals/doip.md`.
>
> Source: https://quantexlab.com/en/develop/elm327_doip.html

ELM327 DoIP Extension - ISO 13400 via AT Commands 
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
# ELM327 ISO 13400 (DoIP)

## Overview

ELM327 protocol extension for ISO 13400 (DoIP - Diagnostics over Internet Protocol) support. Protocol D allows performing UDS diagnostics over Ethernet/IP using the standard ELM327 AT interface. 
### How DoIP works

```

Tester (ScanDoc)                    Vehicle
┌──────────┐    TCP/IP     ┌─────────────┐    CAN/LIN    ┌─────┐
│ SA=0E80  │───────────────│ DoIP entity │───────────────│ ECU │
│ (tester) │   Ethernet    │ (gateway)   │ internal bus  │(TA) │
└──────────┘               └─────────────┘               └─────┘

```

DoIP entity - a vehicle network node supporting ISO 13400. It can be a DoIP gateway (a gateway between Ethernet and the internal CAN/LIN buses) or a DoIP edge node (an ECU with its own Ethernet interface). Each DoIP entity has
 a logical address, an IP address, and a VIN. 

Three parameters must be configured: ``````````

| Parameter | Command | What it sets | How to find out |
| --- | --- | --- | --- |
| IP of DoIP entity | AT DI | Where to connect via TCP | AT DD (discovery) or set manually |
| Source Address | AT DS | Logical address of the tester | Default 0E80, usually does not need to be changed |
| Target Address | AT DT | Logical address of the target ECU | From the vehicle documentation |

If `DI` is not set (IP = `00000000`), `AT SP D` automatically performs discovery (UDP broadcast), finds the first DoIP entity and fills the parameters from its response: 
- `DI` ← IP address of the DoIP entity (e.g. `C0A80C04` = 192.168.12.4) 
- `DT` ← logical address of the DoIP entity (e.g. `3828` - typically a gateway) 
- `DS` ← remains `0E80` (default) 

After that, a TCP connection → Routing Activation → ready for UDS. 
## Examples

### Request to the gateway

`AT SP D` finds the DoIP entity and connects on its own: 

```

>ATZ
ELM327 v2.3

>ATE0
OK

>AT SP D                 # discovery → DI=C0A80C04, DT=3828, DS=0E80
OK                       # TCP + Routing Activation completed

>22 F1 90                # UDS Read VIN - request goes to the gateway (DT=3828)
62 F1 90 54 4D 42 4A 43 37 4E 59 33 50 46 30 32 30 36 30 38

```

### Request to a specific ECU behind the gateway

For example, the engine control unit: 

```

>ATZ
ELM327 v2.3

>ATE0
OK

>AT SP D                 # discovery + connection to the gateway
OK

>22 F1 90                # VIN request to the gateway (DT=3828)
62 F1 90 54 4D 42 4A 43 37 4E 59 33 50 46 30 32 30 36 30 38

>AT DT 0010              # switch Target Address to the engine ECU
OK

>22 F1 90                # same request - now it goes to ECU 0010 via the gateway
62 F1 90 ...

```

## DoIP AT commands

### DI hhhhhhhh - IP address of the DoIP entity

Sets the IPv4 address of the ECU (DoIP entity) in hex format (4 bytes, big-endian). 

```

>AT DI C0A80C04
OK

```

Example: `C0A80C04` = 192.168.12.4 

Default value: `00000000` (not set). If the IP is not set when `AT SP D` is issued, automatic Vehicle Discovery (UDP broadcast) is performed. 
### DS hhhh - Source Address

Sets the logical address of the tester (Source Address, SA) in hex format (2 bytes). 

```

>AT DS 0E00
OK

```

Range of tester addresses per ISO 13400: `0x0E00`–`0x0FFF`. Default value: `0E80`. 
### DT hhhh - Target Address

Sets the logical address of the ECU (Target Address, TA) in hex format (2 bytes). 

```

>AT DT 3828
OK

```

Range of ECU addresses per ISO 13400: `0x0001`–`0x0DFF`. Default value: `0001`. 
### DA hh - Routing Activation Type

Sets the Routing Activation Type. Must be set before `AT SP D` - it is sent to the DoIP entity in the Routing Activation Request during connection. It defines the level of diagnostic access being requested. Change it
 only if the gateway rejects connection with type `00` (Default). 

```

>AT DA 00
OK

```
``````

| Value | Description |
| --- | --- |
| 00 | Default - standard diagnostics (default) |
| 01 | WWH-OBD - access only to data required by regulation (emissions) |
| E0 | Central Security - extended access through the central security gateway |

### DC hhhh - DoIP TCP port

Sets the TCP port of the DoIP entity in hex format. Must be set before `AT SP D`. Default is `3458` (13400 decimal) - the standard port per ISO 13400. Change it only if the DoIP entity uses a non-standard
 port.
 

```

>AT DC 3458
OK

```

### DD - DoIP entity discovery

Performs UDP broadcast Vehicle Discovery (ISO 13400 Vehicle Identification Request). Prints a list of found DoIP entities with their VIN, logical address and IP. 

Automatically fills `DI` (IP) and `DT` (Target Address) from the first discovered DoIP entity. 

When `DI=0`, `AT SP D` performs discovery on its own - a separate `AT DD` is not required. `AT DD` is useful when you need to see which DoIP entities are on the network (VIN, addresses,
 IP) before connecting. 

```

>AT DD
VIN:WBAPH5C55BA123456
ADDR:3828
IP:C0A80C04
OK

```

Multiple DoIP entities on the network: 

```

>AT DD
VIN:WBAPH5C55BA123456
ADDR:3828
IP:C0A80C04
VIN:WF0XXXGCDX1234567
ADDR:1010
IP:C0A80C05
OK

```

If no DoIP entities are found: 

```

>AT DD
NO DATA

```

## Initialization sequence
````````````````````

| Scenario | Sequence |
| --- | --- |
| Automatic (gateway) | ATZ → AT SP D |
| ECU behind gateway (IP via discovery) | ATZ → AT DD → AT DT xxxx → AT SP D |
| ECU behind gateway (IP known) | ATZ → AT DI hhhhhhhh → AT DT xxxx → AT SP D |

### What happens on AT SP D

- Disconnect - disconnecting the current protocol 
- Connect - creating the ISO 13400 J2534 channel 
- Discovery (if IP is not set) - UDP broadcast to find the DoIP entity 
- Set Config - setting IP, SA, TA, port, activation type 
- TCP Connect - establishing the TCP connection to the DoIP entity 
- Routing Activation - DoIP routing activation 

If any step fails, `UNABLE TO CONNECT` is returned. 
## OBD data format

### Sending

Hex data is sent as the UDS payload. ELM327 automatically wraps it into a DoIP Diagnostic Message with an SA+TA header. 

```

>22F190
62 F1 90 57 42 41 50 48 35 43 35 35 42 41 31 32 33 34 35 36

```

### Response (H0 - headers off, default)

Only the UDS payload, without the SA/TA header: 

```

62 F1 90 57 42 41 ...

```

### Response (H1 - headers on)

SA + TA (4 bytes) + UDS payload: 

```

38 28 0E 00 62 F1 90 57 42 41 ...

```

Header format: `[TA_hi] [TA_lo] [SA_hi] [SA_lo]` - address of the response sender (ECU → Tester). 
## Automatic protocol search

DoIP is included in the automatic protocol search order (`AT SP 0`). On a DoIP attempt: 
- A temporary ISO 13400 channel is created 
- Vehicle Discovery is performed (UDP broadcast, 2 sec timeout) 
- If a DoIP entity is found - connect via TCP + Routing Activation 
- If the connection is successful - a probe OBD request is sent 
- If no DoIP entity is found or there is no response - proceed to the next protocol 
## Parameter reset

The `AT D` (defaults) and `AT Z` (reset) commands reset all DoIP parameters: 
- `DI` → `00000000`
- `DS` → `0E80`
- `DT` → `0001`
- `DA` → `00`
- `DC` → `3458` (13400) 
## Limitations

- Only one DoIP entity is supported at a time 
- Discovery finds up to 16 unique DoIP entities (deduplicated by logical address), but only one is connected to 
- TLS (secured DoIP) is not supported 
- Maximum UDS message size: 4095 bytes 
-  up 

Select a region to match content and online products to your location. 
Europe 
Russia 
Rest World
