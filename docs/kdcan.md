# K+DCAN UART mode (`kdcan`)

OpenDIAG can act as a **transparent K-Line UART** so browser tools such as
[BMWeb](https://www.e46fanatics.com/threads/bmweb-diagnostics-release-inpa-web.1344099/)
drive the vehicle bus the same way they drive a dumb K+DCAN cable: raw bytes,
host-selected baud/parity, and host-timed break for fast init / 5-baud.

## Scope (v1)

| In v1 | Deferred / caveats |
| --- | --- |
| USB data CDC ↔ K-Line byte pipe (`BUS_KLINE_FRAME_RAW`) | BLE as a first-class BMWeb path (no line coding / break) |
| CDC line coding → baud / parity / data bits | CAN / D-CAN PHY bridging (use `slcan` for raw CAN) |
| CDC Send_Break → K-Line break assert/deassert | CanAdapterElm binary (`ediabas`) |
| Session latch so DTR+RTS idle does not drop the link | |

Default on enter: **115200 8N1**, K-line only, checksums off.

## Switching on

```text
link usb
mode kdcan
```

From ELM327: `AT VIF KDCAN`. Or short-tap the button until the LED is **blue**.

Then open the **data** port in the host — the USB interface named **COM** /
`*-if00` — not the DEBUG console (`*-if02`). Prefer **USB**; BLE can still
carry sticky `kdcan` bytes at the last UART settings, but BMWeb expects CDC
line coding and break.

## DTR / session latch

Classic K+DCAN tools (including BMWeb) often leave **DTR and RTS both low**
while the session is idle. On a normal USB-serial cable that is fine; on
OpenDIAG those lines used to mean “port closed” and would tear down the data
link.

In `kdcan` mode the firmware **latches** the data-port session on line-state
change, line-coding change, or first RX. The latch clears only on USB unplug,
leaving `kdcan`, or `link drop` — **not** when DTR+RTS go low. Host DTR is
never used as K-Line TX-enable.

## BMWeb smoke checklist

1. `mode kdcan` (and `link usb`)
2. Close any prior serial handle; reconnect BMWeb to the OpenDIAG **data** COM
3. Confirm one bind on the debug console (no CDC0 flap storm across DTR idle)
4. Identify a known module (e.g. DS2 at 9600 8E1); baud changes via line coding
5. Confirm physical echo: a quiet, powered bus should not report `IFH-0003`

## Implementation

- Front-end: [`main/kdcan.c`](../main/kdcan.c)
- K-Line RAW framing and `BUS_IOCTL_BREAK_SET`: [`main/bus.h`](../main/bus.h),
  [`main/kline.c`](../main/kline.c)
- CDC latch / line coding / break: [`main/usb_cdc.c`](../main/usb_cdc.c)
