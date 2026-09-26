# P4 USB Display — Guition JC8012P4A1C as a wired Linux monitor

The Guition JC8012P4A1C (ESP32-P4 + ESP32-C6, 10.1" 800×1280 IPS MIPI-DSI) as a
real extra monitor for a Linux laptop over one USB 2.0 cable. The desktop sees
it as a normal display (KDE: output `DVI-I-1`, "P4 USB Panel"), with orientation,
placement and power handled by the compositor like any other monitor.

```
KWin ──> EVDI virtual connector ──> evdi_display.py ──USB 2.0 HS──> P4 ──> JD9365 panel
         (DRM, EDID 800x1280)       4:2:0 JPEG            bulk      HW JPEG decode
                                    (libjpeg-turbo)                 into DPI frame buffer
```

Board support (panel init, DSI, PSRAM, the chip revision) comes from
`~/Downloads/p4dash` — read its `21.md` "Traps" section before touching anything
display-related. This repo adds the USB link and the Linux side.

## Hardware

| USB-C port | What it is | Used for |
|---|---|---|
| upper | P4 USB-Serial-JTAG, `/dev/ttyACM0` | flashing, logs, RTS reset |
| **middle** | P4 USB 2.0 **High-Speed OTG** (`303a:4020` when running) | the display link |
| lower | CH340 USB-UART | flashing (RTS reset does nothing here) |

The middle port alone powers the board and carries the display.
ESP32-P4 silicon is **v1.3** (pre-v3): builds need `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`.

## Layout

| | |
|---|---|
| `firmware/p4usbdisp/` | the display firmware |
| `firmware/usb_hs_test/` | bare HS bulk throughput test (kept as a reference measurement) |
| `host/evdi_display.py` | the daemon: EVDI monitor → JPEG → USB |
| `host/edid.py` | EDID for the virtual connector (800×1280, 135×217 mm) |
| `host/p4disp.py` | host side of the wire protocol |
| `host/mirror.py` | earlier approach: mirror a screen via the Wayland portal |
| `host/test_card.py` | animated test card, for checking the board without a desktop |
| `host/usb_hs_bench.py` | host half of `usb_hs_test` |
| `host/system/` | udev rules, modprobe config, systemd user unit |

## Measured

| | |
|---|---|
| USB bulk OUT into internal RAM | ~30 MB/s |
| HW JPEG decode, 800×1280 → RGB565 | 10.4 ms (typical desktop), 18 ms (noise) |
| Sustained display rate, test card | 60 fps, zero decode failures |
| Ceiling, small frames | ~82 fps (decode and next receive serialise on memory bandwidth) |
| Worst case (755 KB noise frames) | 29 MB/s, every frame shown |
| Host encode (simplejpeg, BGRX, no rotation) | ~2–5 ms |

## Setup

**Firmware** (ESP-IDF v5.5.5 at `~/esp/esp-idf`):

```bash
cd firmware/p4usbdisp && . ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyACM0 flash
```

Boots to a colour-bar test card, then shows whatever arrives over USB.

**Host:**

```bash
cd host && python3 -m venv --system-site-packages .venv && .venv/bin/pip install pyusb pillow numpy simplejpeg
sudo apt install evdi-dkms libevdi1 libevdi-dev
```

With Secure Boot on, the `evdi-dkms` install asks for a one-time password and
the next boot shows the MOK manager: *Enroll MOK → Continue → Yes →* password.
Miss it and the module will not load; `sudo mokutil --import
/var/lib/shim-signed/mok/MOK.der` and reboot again.

```bash
sudo install -m644 host/system/evdi-modprobe.conf /etc/modprobe.d/evdi.conf
sudo install -m644 host/system/evdi-modules-load.conf /etc/modules-load.d/evdi.conf
sudo install -m644 host/system/70-evdi-uaccess.rules host/system/70-p4-usb-display.rules /etc/udev/rules.d/
sudo udevadm control --reload
cp host/system/p4-usb-display.service ~/.config/systemd/user/ && systemctl --user daemon-reload && systemctl --user enable p4-usb-display
```

From then on, plugging the board in starts the daemon and the monitor appears;
unplugging removes it. Set orientation in KDE's display settings (or
`kscreen-doctor output.DVI-I-1.rotation.none|left|right|inverted`) — KDE
remembers it per monitor.

```bash
systemctl --user status p4-usb-display
journalctl --user -u p4-usb-display -f
```

## Wire protocol

Vendor-class interface 0, bulk OUT `0x01`. Each frame is two writes: a 16-byte
header `{u32 magic "P4D1", u8 type, u8 flags, u16 reserved, u32 len, u32 seq}`
(little-endian; a short packet, so it ends the device-side transfer), then `len`
bytes of baseline 4:2:0 JPEG at exactly 800×1280. The host resets the USB port
before streaming, which resynchronises the device's parser. Vendor control
requests: `0x01` IN = stats, `0x02` OUT = backlight (`wValue` 0–100).

The board always decodes the newest complete frame; anything that arrives while
it is busy is dropped, never queued, so latency cannot build up.

## Traps — read before debugging anything

The p4dash traps all still apply (revised panel glass, 4:2:0 only, BGR order,
PSRAM cache flush, early silicon). These are the new ones.

**USB DMA straight into PSRAM is 20× slower than into internal RAM.** 1.5 MB/s
against ~30 MB/s. TinyUSB's DWC2 driver invalidates the cache over every received
range, and on PSRAM that costs ~11.5 ms per transfer regardless of size. The
symptom is every host write taking ~12 ms, including a 16-byte header. Receive
into internal-RAM bounce buffers and `memcpy` into PSRAM (re-arm the other bounce
buffer before copying, so the copy overlaps the next transfer), then one
`esp_cache_msync(C2M)` over the finished frame for the JPEG engine's DMA.

**Never M2C-invalidate a PSRAM frame slot in the hot path.** An explicit
`esp_cache_msync(M2C)` over a 35 KB PSRAM slot before decoding cost ~200 ms per
frame and throttled the board to 5 fps while the log said decode took 13 ms.
It is redundant anyway: the driver invalidates each received range.

**TinyUSB's stock vendor class caps out around 7 MB/s.** It funnels everything
through a 512-byte FIFO with a copy. An app-level class driver
(`usbd_app_driver_get_cb`) that arms large transfers directly reaches ~30 MB/s.

**Vendor control requests never reach a class driver.** TinyUSB routes every
`bmRequestType.type == VENDOR` request to the global `tud_vendor_control_xfer_cb`,
regardless of recipient. Handling it in the driver's `control_xfer_cb` gets you a
stall and `[Errno 32] Pipe error` on the host.

**The small-frame ceiling is decode, not USB.** While the JPEG engine runs, the
next frame's final transfer does not complete, so small frames serialise: ~1.3 ms
receive + ~10.4 ms decode ≈ 82 fps. Big frames overlap and hide it. Irrelevant at
60 Hz, but it is why the numbers do not add up the way you would expect.

**Advertise the panel's native portrait raster and let the compositor rotate.**
Rotating on the CPU costs ~6 ms per frame, four times the JPEG encode. With an
800×1280 EDID, KWin renders any orientation on the GPU and frames arrive in panel
order. Consequence: never set rotation from the daemon by default — it will fight
the user's choice (`--rotate-output` exists for first-time setup only).

**uaccess rules must sort before `73-seat-late.rules`.** A `99-*.rules` file with
`TAG+="uaccess"` silently does nothing; the device stays root-only and pyusb
reports "The device has no langid". Hence the `70-` prefixes.

**A device added before its udev rule keeps the old permissions.** Re-plug, reset
the board, or `udevadm trigger` after installing a rule.

**`pkill -f <script name>` from a shell whose command line contains that name
kills the shell too.** Stop the daemon with `systemctl --user stop`.

**Stopping the daemon removes the monitor.** `evdi_disconnect` unplugs the
virtual connector, so KDE reflows windows off it. Expected, but surprising the
first time when restarting the service.

## Not done

- **Touch.** The GSL3680 driver in p4dash plus a USB HID touchscreen interface
  would do it; KDE maps touchscreens to their output, so orientation would follow.
- **Tearing.** Frames decode straight into the single scanned frame buffer (the
  p4dash-proven path). A second frame buffer with a vsync flip would remove the
  occasional tear line on fast motion.
- **Real USB PID.** `303a:4020` is an unallocated development PID.
