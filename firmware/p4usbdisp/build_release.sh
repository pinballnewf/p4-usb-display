#!/usr/bin/env bash
# Build both firmware variants as single flashable images in ../../dist/:
#   p4usbdisp-rev1.bin  ESP32-P4 silicon v1.x (tested)
#   p4usbdisp-rev3.bin  ESP32-P4 silicon v3.x (builds; untested on hardware)
# Each is a merged image (bootloader + partition table + app) written at 0x0.
# Needs ESP-IDF v5.5.x sourced (. ~/esp/esp-idf/export.sh).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
DIST="$(cd ../.. && pwd)/dist"
mkdir -p "$DIST"

build() {  # build <name> <build dir> <sdkconfig defaults list>
    local name="$1" dir="$2" defaults="$3"
    idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" set-target esp32p4 >/dev/null
    idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" build
    idf.py -B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" merge-bin -o "$DIST/$name"
    echo "built $DIST/$name"
}

build p4usbdisp-rev1.bin build-rev1 "sdkconfig.defaults"
build p4usbdisp-rev3.bin build-rev3 "sdkconfig.defaults;sdkconfig.defaults.rev3"
(cd "$DIST" && sha256sum p4usbdisp-rev*.bin > SHA256SUMS && cat SHA256SUMS)
