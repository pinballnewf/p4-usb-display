# Local patches to TinyUSB 0.21.0~2

Vendored from the ESP-IDF component registry (`espressif/tinyusb` 0.21.0~2) and
used through `override_path` in `main/idf_component.yml`. Trimmed to what the
ESP-IDF build compiles (`src/` minus other chips' ports, `lib/networking`).
Every change is marked `P4USBDISP PATCH` in the source.

## DWC2: interval-aware isochronous OUT parity

`src/portable/synopsys/dwc2/dcd_dwc2.c`

**Problem.** When arming an isochronous endpoint, `edpt_schedule_packets()`
targets the (micro)frame parity of *the next* frame. That is only right for an
interval of 1, and only when the completion interrupt was serviced within the
same microframe:

- at `bInterval` >= 2 on high speed (an even number of microframes), every
  packet lands on the same parity as the last one, so every re-arm targets the
  wrong parity - reception stops after the first packet or two;
- at interval 1, a completion interrupt delayed past the microframe boundary
  (here: while the P4's JPEG engine saturates memory) re-arms for the frame
  after next, and one packet is silently lost.

**Change.**

1. `xfer_ctl_t` remembers the parity each isochronous OUT arm targeted, and
   whether a received packet has confirmed it.
2. For isochronous OUT endpoints with an even interval, re-arm for the
   confirmed parity. Interval-1 endpoints keep the stock behaviour.
3. The incomplete-isochronous-OUT interrupt (`GINTSTS.INCOMPISOOUT`, previously
   masked and unhandled) is enabled; for an even-interval endpoint that missed
   its frame, the parity is flipped and marked unconfirmed, so a wrong first
   guess recovers instead of leaving the endpoint deaf.
4. The remembered parity is reset whenever the endpoint is (re)activated.

**Effect here.** The UAC2 speaker runs at `bInterval` 4 (1 ms) on high speed,
giving a late interrupt ~875 us to re-arm instead of none: zero lost audio
packets under full display load, where interval 1 lost ~15-50 per second.

Upstream TinyUSB (master, checked 2026-09-25) has the same arming code.
