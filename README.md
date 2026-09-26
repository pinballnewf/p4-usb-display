# P4 USB Display

Turn a **Guition JC8012P4A1C** (the 10.1" ESP32-P4 "smart panel", 800×1280 IPS
with capacitive touch) into a **plug-and-play USB monitor for Linux** — one
USB-C cable carries the picture, the touchscreen and the speaker.

Plug it in and your desktop gets:

- **a real extra monitor** — shows up in your display settings like any other
  screen; extend or mirror, portrait or landscape, arrange it wherever you like;
- **multi-touch** — tap, drag, two-finger scroll and pinch, mapped to that screen;
- **a speaker** — a normal "P4 USB Display" sound output with hardware volume.

Unplug it and the monitor goes away. No network, no Wi-Fi, no app to keep open.

| | |
|---|---|
| Frame rate | as fast as the desktop draws it — ~57 fps of continuous motion on KDE Plasma |
| Latency | ~20–35 ms from the compositor to the glass, about the same as a typical monitor |
| Resolution | the panel's native 800×1280 (portrait) / 1280×800 (landscape) |
| Audio | 48 kHz, glitch-free under full display load |

## What you need

**The board:** a Guition **JC8012P4A1C** (ESP32-P4 + ESP32-C6, 10.1" JD9365
panel, GSL3680 touch). Two things vary between batches of this board:

- *Chip revision.* Boards so far ship early ESP32-P4 silicon (v1.x) — that image
  is the tested one. An image for production v3.x silicon is included but has
  not been tested on hardware; `flash.sh` picks the right one automatically.
- *Panel glass.* Guition revised the glass; this firmware uses the init table for
  the **revised** panel. If yours shows the colour-bar test card dark or washed
  out after flashing, see [Troubleshooting](#troubleshooting).

**The computer:** Linux with USB 2.0 (any USB-A or USB-C port; a C-to-A adapter
is fine). Tested on **Ubuntu 26.04, KDE Plasma 6 (Wayland)**. The display uses
[EVDI](https://github.com/DisplayLink/evdi) (the open-source part of DisplayLink
support), so it should work with other desktops that support DisplayLink
monitors; the automatic touchscreen mapping is KDE-only for now.

**A USB-C cable** — the board has three USB-C ports along one edge:

| Port | Used for |
|---|---|
| upper | flashing the firmware (once) |
| **middle** | **the display link — this is the one you use day to day** |
| lower | not used |

## Install

### 1. Flash the firmware (once)

Connect the board's **upper** USB-C port, then:

```bash
git clone https://github.com/pinballnewf/p4-usb-display.git
cd p4-usb-display
./flash.sh
```

It finds the board, checks the chip revision, downloads the matching image from
the latest release and flashes it (it sets up its own copy of `esptool` if you
don't have one). Add `--backup` to save the board's current firmware first.
If it reports a permission problem, add yourself to the `dialout` group
(`sudo usermod -aG dialout $USER`) and log in again.

The board restarts into a colour-bar test card. You can now unplug the upper port.

### 2. Install the host side

```bash
./install.sh
```

This installs EVDI and a few Python packages, adds two udev rules, and sets up a
small user service that starts whenever the board is plugged in. It asks for
your password for the system parts.

> **Secure Boot:** EVDI is a kernel module, so on a Secure Boot machine it has to
> be signed with a key your firmware trusts. If you don't already have one
> enrolled (for example from NVIDIA drivers), the installer explains the steps:
> apt asks you to pick a one-time password, and on the next reboot a blue
> *MOK management* screen appears — choose **Enroll MOK → Continue → Yes**, type
> the password, then **Reboot**. It's a one-time step.

### 3. Plug it in

Connect the board's **middle** USB-C port. Within a few seconds a new
**P4 USB Panel** monitor appears. Arrange and rotate it in your display settings
(KDE: *System Settings → Display & Monitor*); your choice is remembered.

## Using it

- **Orientation:** set it in your display settings — the panel reports itself in
  its native portrait layout and the compositor rotates it on the GPU at no cost.
  From a terminal on KDE: `kscreen-doctor output.DVI-I-1.rotation.left` (or
  `none`, `right`, `inverted`; the output name may differ — `kscreen-doctor -o`).
- **Sound:** pick *P4 USB Display* as the output in your sound settings. The
  panel has one speaker, so stereo is mixed to mono.
- **Touch:** works on the panel straight away on KDE. Elsewhere, map the
  touchscreen named *P4 USB Display* to the new monitor in your desktop's
  touchscreen settings.
- **Brightness:** the backlight turns off when the desktop blanks the screen.

Status and logs:

```bash
systemctl --user status p4-usb-display
journalctl --user -u p4-usb-display -f
```

The log prints a line every few seconds with the frame rate, per-frame timings
and audio health, which is the first thing to check if something feels off.

## Troubleshooting

**Nothing appears when I plug in the middle port.**
Check the board is seen: `lsusb -d 303a:4020` should list *P4 USB Display*. If
it lists `303a:1001` instead, the display firmware isn't flashed (step 1). If it
lists nothing, try another cable (some are charge-only). Then check
`systemctl --user status p4-usb-display` and the log.

**"no free EVDI device" in the log.** The EVDI module isn't loaded. After a fresh
install on a Secure Boot machine this means the key hasn't been enrolled yet —
reboot and complete the MOK screen. Otherwise `sudo modprobe evdi` and check
`sudo dmesg | grep -i evdi`.

**The test card is dark or washed out.** You probably have the original panel
glass rather than the revised one. Building from source with the driver's
built-in init table usually fixes it — see
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#board-traps-inherited-from-the-authors-earlier-projects-on-this-board).

**Touches land on the wrong screen (not KDE).** Map the *P4 USB Display*
touchscreen to the new monitor in your desktop's settings.

**Power.** The middle port alone powers the board. If the backlight flickers or
the board resets on a weak port, use a powered hub or a port that supplies more
current.

## Uninstall

```bash
./install.sh --uninstall
```

The EVDI packages are left in place (other software may use them); the script
prints the command to remove them.

## How it works

The Linux side creates a virtual monitor with EVDI and hands it an EDID
describing the panel. Every frame your compositor draws for that monitor is
JPEG-compressed (libjpeg-turbo, ~2–5 ms) and sent over USB 2.0 High-Speed. On
the board, the ESP32-P4's hardware JPEG decoder writes it straight into the
panel's frame buffer in ~10.5 ms. Touch is a standard USB multi-touch HID device
and sound a standard USB Audio Class 2 speaker, so Linux drives both with its
built-in drivers.

The details — the USB protocol, measurements, and a long list of hardware and
driver traps found along the way (including a patch to TinyUSB's USB driver that
fixed audio crackle under video load) — are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md), along with instructions for building
the firmware from source.

## Credits

- [EVDI](https://github.com/DisplayLink/evdi) — virtual monitors on Linux.
- [ESP-IDF](https://github.com/espressif/esp-idf) and
  [TinyUSB](https://github.com/hathach/tinyusb) (vendored with a local patch).
- The GSL3680 touch firmware blob comes from
  [kvj's ESPHome component](https://github.com/kvj/esphome), itself derived from
  the vendor's Linux driver.
- Board schematics:
  [p1ngb4ck/unofficial_guition_esp32p4_repo](https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo).

Not affiliated with Guition or Espressif. The USB ID `303a:4020` is an
unallocated development ID under Espressif's vendor ID.

## License

[GPL-3.0](LICENSE). The vendored TinyUSB is MIT-licensed
(`firmware/p4usbdisp/components/espressif__tinyusb/LICENSE`).
