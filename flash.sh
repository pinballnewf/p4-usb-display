#!/usr/bin/env bash
# Flash the P4 USB Display firmware onto a Guition JC8012P4A1C.
#
#   ./flash.sh                 download the latest release image and flash it
#   ./flash.sh --file X.bin    flash a local merged image instead
#   ./flash.sh --backup        save the board's current 16 MB flash first
#   ./flash.sh --port /dev/ttyACM0
#
# Connect the board's UPPER USB-C port (the ESP32-P4's USB-Serial-JTAG). The
# middle port is the display link and cannot flash; the lower one (CH340) can,
# but cannot reset the chip, so this script looks for the upper one.
set -euo pipefail

RELEASE_URL="https://github.com/pinballnewf/p4-usb-display/releases/latest/download"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/p4-usb-display"
PORT="" FILE="" BACKUP=false

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)   PORT="$2"; shift 2 ;;
        --file)   FILE="$2"; shift 2 ;;
        --backup) BACKUP=true; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
done

# ---- esptool -------------------------------------------------------------------
if command -v esptool >/dev/null; then
    ESPTOOL=(esptool)
elif command -v esptool.py >/dev/null; then
    ESPTOOL=(esptool.py)
else
    if [[ ! -x "$CACHE/esptool-venv/bin/esptool" ]]; then
        say "installing esptool into $CACHE/esptool-venv"
        mkdir -p "$CACHE"
        python3 -m venv "$CACHE/esptool-venv"
        "$CACHE/esptool-venv/bin/pip" install -q "esptool>=4.8"
    fi
    ESPTOOL=("$CACHE/esptool-venv/bin/esptool")
fi

# ---- find the board ------------------------------------------------------------
if [[ -z "$PORT" ]]; then
    for tty in /sys/class/tty/ttyACM*; do
        [[ -e "$tty" ]] || continue
        dev="$(readlink -f "$tty/device/..")"
        if [[ "$(cat "$dev/idVendor" 2>/dev/null)" == "303a" && "$(cat "$dev/idProduct" 2>/dev/null)" == "1001" ]]; then
            PORT="/dev/$(basename "$tty")"
            break
        fi
    done
fi
[[ -n "$PORT" ]] || die "no ESP32-P4 USB-Serial-JTAG port found - connect the board's UPPER USB-C port (or pass --port)"
[[ -w "$PORT" ]] || die "no permission for $PORT - add yourself to the 'dialout' group (sudo usermod -aG dialout \$USER) and log in again"
say "board on $PORT"

# ---- chip revision -> image ----------------------------------------------------
# Pre-v3 and v3+ ESP32-P4 silicon are not binary compatible (different IDF
# build option), so the image must match the chip.
info="$("${ESPTOOL[@]}" --chip esp32p4 -p "$PORT" chip_id 2>&1)" || { echo "$info"; die "could not talk to the chip"; }
rev="$(grep -oE 'revision v[0-9]+\.[0-9]+' <<<"$info" | head -1 | cut -dv -f2)"
[[ -n "$rev" ]] || { echo "$info"; die "could not read the chip revision"; }
major="${rev%%.*}"
if (( major < 3 )); then
    image="p4usbdisp-rev1.bin"
else
    image="p4usbdisp-rev3.bin"
    warn "ESP32-P4 revision v$rev: the v3 image builds but has not been tested on hardware"
fi
say "chip revision v$rev -> $image"

if [[ -z "$FILE" ]]; then
    mkdir -p "$CACHE"
    FILE="$CACHE/$image"
    say "downloading $image from the latest release"
    curl -fL --progress-bar -o "$FILE.tmp" "$RELEASE_URL/$image" || die "download failed ($RELEASE_URL/$image)"
    mv "$FILE.tmp" "$FILE"
fi
[[ -f "$FILE" ]] || die "no such file: $FILE"

# ---- flash ---------------------------------------------------------------------
if $BACKUP; then
    out="p4-flash-backup-$(date +%Y%m%d-%H%M%S).bin"
    say "backing up the current 16 MB flash to $out (about 90 s)"
    "${ESPTOOL[@]}" --chip esp32p4 -p "$PORT" -b 921600 read_flash 0 0x1000000 "$out"
fi

say "flashing $FILE"
"${ESPTOOL[@]}" --chip esp32p4 -p "$PORT" -b 921600 write_flash 0x0 "$FILE"

echo
say "done. The board restarts into a colour-bar test card."
echo "    Connect its MIDDLE USB-C port to use it as a display (run ./install.sh first)."
