# Ediabas / Deep OBD mode (E46 DS2)

OpenDIAG can speak the **Deep OBD / CanAdapterElm binary** wire protocol so
[EdiabasLib](https://github.com/uholeschak/ediabaslib), Deep OBD, and INPA/Tool32
(via EdiabasLib `api32`) drive BMW **DS2** without host changes.

## Scope (v1)

| In v1 | Deferred |
| --- | --- |
| Adapter detect (`FE` / `FD` / escape / `FB` / `FC`) | BMW-FAST K-Line at 115200 |
| K-Line setup telegrams `0x00` / `0x02` | D-CAN BMW-FAST / TP2.0 / ISO-TP |
| BMW **DS2** (typically 9600 8E1) | VAG KWP1281 / TP2.0 |
| E46 with **OBD pin 7 bridged to pin 8** | ENET / HSFZ |

Adapter identity reported to the host:

- Type `0x0046`, version `0x000F` (ConfigTool requires version ≥ 15)

## Switching on

Transport and grammar are separate:

```text
link usb          # or: link ble
mode ediabas
```

From ELM327: `AT VIF EDIABAS`.

Then open the **data** port (not the debug console) in Deep OBD / EdiabasLib at
**115200** baud. USB CDC ignores host baud; BLE is already framed by GATT.

## Hardware note (E46)

Classic E46 modules use two K-Lines on OBD pins **7** and **8**. Stock OpenDIAG
drives pin 7 only; a **hardware bridge 7↔8** is required for body modules.
Software `bridge78` remains a no-op on stock boards.

## Host smoke checklist

1. `mode ediabas` (and `link usb` or `link ble`)
2. EdiabasLibConfigTool → Check Connection → **Custom**
3. Deep OBD → adapter type Custom
4. Identify E46 DME (address `0x12`) and one body module that needs pin 8

## Implementation

- Front-end: [`main/ediabas.c`](../main/ediabas.c), BMW-FAST helpers in
  [`main/ediabas_bmwfast.c`](../main/ediabas_bmwfast.c)
- Host tests: `test/host/suites/test_ediabas.c`
- Wire reference: ediabaslib `CanAdapterElm.X/main.c` and
  `EdCustomAdapterCommon.cs`
