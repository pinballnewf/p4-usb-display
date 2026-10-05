#!/usr/bin/env bash
# Host-side installer for the P4 USB Display (Debian/Ubuntu, Arch/CachyOS).
#
#   ./install.sh              install or update
#   ./install.sh --uninstall  remove everything this script installed
#
# Run as your normal user; it asks for sudo for the system parts.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$HOME/.local/share/p4-usb-display"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT="p4-usb-display.service"
HOST_FILES=(evdi_display.py edid.py p4disp.py mirror.py test_card.py requirements.txt)
APT_PKGS=(evdi-dkms libevdi1 python3-venv python3-gi usbutils)
# Arch: evdi-dkms (module + libevdi) is AUR-only; these come from the repos.
PACMAN_PKGS=(dkms python python-gobject usbutils base-devel git)

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

[[ $EUID -ne 0 ]] || die "run as your normal user, not root (sudo is used where needed)"
if command -v apt-get >/dev/null; then
    distro=debian
elif command -v pacman >/dev/null; then
    distro=arch
else
    die "this installer supports Debian/Ubuntu (apt) and Arch/CachyOS (pacman); see README for manual steps"
fi

uninstall() {
    say "stopping and removing the user service"
    systemctl --user disable --now "$UNIT" 2>/dev/null || true
    rm -f "$UNIT_DIR/$UNIT"
    rm -rf "$UNIT_DIR/sys-subsystem-usb-p4display.device.wants"
    systemctl --user daemon-reload
    say "removing $APP_DIR"
    rm -rf "$APP_DIR"
    say "removing system configuration (sudo)"
    sudo rm -f /etc/modprobe.d/evdi.conf /etc/modules-load.d/evdi.conf \
               /etc/udev/rules.d/70-evdi-uaccess.rules /etc/udev/rules.d/70-p4-usb-display.rules
    sudo udevadm control --reload
    say "done. The EVDI packages were left installed; remove them with:"
    if [[ $distro == arch ]]; then
        echo "    sudo pacman -Rns evdi-dkms"
    else
        echo "    sudo apt remove evdi-dkms libevdi1"
    fi
}

if [[ "${1:-}" == "--uninstall" ]]; then
    uninstall
    exit 0
fi

# ---- 1. packages -----------------------------------------------------------
secure_boot=false
if command -v mokutil >/dev/null; then
    mokutil --sb-state 2>/dev/null | grep -q enabled && secure_boot=true
elif sb=$(od -An -tu1 -j4 -N1 /sys/firmware/efi/efivars/SecureBoot-* 2>/dev/null); then
    [[ ${sb// /} == 1 ]] && secure_boot=true
fi

install_debian_pkgs() {
    if ! dpkg -s evdi-dkms >/dev/null 2>&1 && $secure_boot; then
        warn "Secure Boot is on. Installing evdi-dkms builds a kernel module that must be"
        warn "signed with a key your firmware trusts. If this machine has no enrolled key"
        warn "yet, apt will ask you to choose a one-time password, and on the next reboot"
        warn "a blue 'MOK management' screen appears: choose Enroll MOK -> Continue -> Yes,"
        warn "type that password, then Reboot. Without that step the module will not load."
        read -r -p "Continue? [y/N] " a; [[ "$a" == [yY]* ]] || exit 1
    fi
    say "installing packages: ${APT_PKGS[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y "${APT_PKGS[@]}"
}

# DKMS can only build EVDI for kernels whose headers are installed. On Arch the
# headers are a separate package, and if the kernel is held back (IgnorePkg)
# while its headers are not, they drift apart and the running kernel gets no
# module. Report that precisely instead of letting modprobe fail later.
check_arch_headers() {
    local kver kpkg kpkgver
    kver=$(uname -r)
    [[ -e /usr/lib/modules/$kver/build/Makefile ]] && return 0
    kpkg=$(pacman -Qqo "/usr/lib/modules/$kver/vmlinuz" 2>/dev/null) || kpkg=""
    warn "no kernel headers for the running kernel ($kver), so DKMS cannot build"
    warn "the EVDI module for it."
    if [[ -n $kpkg ]]; then
        kpkgver=$(pacman -Q "$kpkg" | cut -d' ' -f2)
        if pacman -Q "$kpkg-headers" >/dev/null 2>&1; then
            warn "  $kpkg is $kpkgver but $kpkg-headers is $(pacman -Q "$kpkg-headers" | cut -d' ' -f2)."
            grep -qE "^[[:space:]]*IgnorePkg.*\b$kpkg\b" /etc/pacman.conf &&
                warn "  $kpkg is held in IgnorePkg (/etc/pacman.conf) but its headers are not."
            warn "  Fix: update the kernel to match (sudo pacman -Syu $kpkg $kpkg-headers"
            warn "  plus any matching module packages such as $kpkg-nvidia-open) and reboot,"
            warn "  or boot a kernel whose headers are installed."
        else
            warn "  Fix: sudo pacman -S $kpkg-headers"
        fi
    fi
    warn "The module will still be built for every other kernel that has headers."
    read -r -p "Continue anyway? [y/N] " a; [[ "$a" == [yY]* ]] || exit 1
}

install_arch_pkgs() {
    if $secure_boot; then
        warn "Secure Boot is on. Arch's DKMS does not sign modules for you: the EVDI"
        warn "module must be signed with a key your firmware trusts (e.g. via sbctl or a"
        warn "MOK set up for your other DKMS modules) or it will not load."
        read -r -p "Continue? [y/N] " a; [[ "$a" == [yY]* ]] || exit 1
    fi
    check_arch_headers
    say "installing packages: ${PACMAN_PKGS[*]}"
    sudo pacman -S --needed --noconfirm "${PACMAN_PKGS[@]}"
    if pacman -Q evdi-dkms >/dev/null 2>&1 || pacman -Q evdi-dkms-git >/dev/null 2>&1; then
        say "evdi-dkms already installed"
        return
    fi
    local helper
    for helper in paru yay; do
        if command -v "$helper" >/dev/null; then
            say "installing evdi-dkms from the AUR with $helper"
            "$helper" -S --needed --noconfirm evdi-dkms
            return
        fi
    done
    say "installing evdi-dkms from the AUR with makepkg"
    local build
    build=$(mktemp -d)
    git clone -q https://aur.archlinux.org/evdi-dkms.git "$build/evdi-dkms"
    (cd "$build/evdi-dkms" && makepkg -si --noconfirm)
    rm -rf "$build"
}

install_${distro}_pkgs

# ---- 2. system configuration ------------------------------------------------
say "installing udev rules and EVDI module configuration"
sudo install -m644 "$REPO/host/system/evdi-modprobe.conf"      /etc/modprobe.d/evdi.conf
sudo install -m644 "$REPO/host/system/evdi-modules-load.conf"  /etc/modules-load.d/evdi.conf
sudo install -m644 "$REPO/host/system/70-evdi-uaccess.rules"   /etc/udev/rules.d/70-evdi-uaccess.rules
# GROUP="plugdev" is a Debian convention; elsewhere udev logs an error for the
# unknown group. Access comes from TAG+="uaccess" either way.
if getent group plugdev >/dev/null; then
    sudo install -m644 "$REPO/host/system/70-p4-usb-display.rules" /etc/udev/rules.d/70-p4-usb-display.rules
else
    sed 's/ GROUP="plugdev",//' "$REPO/host/system/70-p4-usb-display.rules" |
        sudo install -m644 /dev/stdin /etc/udev/rules.d/70-p4-usb-display.rules
fi
sudo udevadm control --reload

needs_reboot=false
# (Read /proc directly: under pipefail, `lsmod | grep -q` can fail spuriously
# when grep exits before lsmod has finished writing.)
if ! grep -q '^evdi ' /proc/modules; then
    if sudo modprobe evdi 2>/dev/null; then
        say "EVDI module loaded"
    else
        if [[ ! -e /usr/lib/modules/$(uname -r)/build/Makefile ]]; then
            warn "the EVDI module could not be loaded: it was not built for the running"
            warn "kernel ($(uname -r)) because its headers are missing (see above)"
        else
            warn "the EVDI module could not be loaded yet (with Secure Boot this usually"
            warn "means the signing key still has to be enrolled at the next reboot)"
        fi
        needs_reboot=true
    fi
elif ! compgen -G '/sys/devices/platform/evdi.*' >/dev/null; then
    # Already loaded (e.g. by DisplayLink software) but without a free device.
    echo 1 | sudo tee /sys/devices/evdi/add >/dev/null && say "added an EVDI device"
fi

# ---- 3. the daemon -----------------------------------------------------------
say "installing the daemon into $APP_DIR"
mkdir -p "$APP_DIR"
for f in "${HOST_FILES[@]}"; do install -m644 "$REPO/host/$f" "$APP_DIR/$f"; done
if [[ ! -x "$APP_DIR/venv/bin/python" ]]; then
    python3 -m venv --system-site-packages "$APP_DIR/venv"
fi
"$APP_DIR/venv/bin/pip" install -q --upgrade pip
"$APP_DIR/venv/bin/pip" install -q -r "$APP_DIR/requirements.txt"

say "installing the user service"
mkdir -p "$UNIT_DIR"
install -m644 "$REPO/host/system/$UNIT" "$UNIT_DIR/$UNIT"
systemctl --user daemon-reload
systemctl --user enable "$UNIT" >/dev/null 2>&1 || true

# ---- 4. start it if the board is already here --------------------------------
if lsusb -d 303a:4020 >/dev/null 2>&1; then
    # Re-run the rules so an already-plugged board gets its permissions and
    # the device unit that starts the service.
    sudo udevadm trigger --action=add --attr-match=idVendor=303a --attr-match=idProduct=4020
    sleep 2
    systemctl --user restart "$UNIT" || true
    say "board found - the display should appear within a few seconds"
elif lsusb -d 303a:1001 >/dev/null 2>&1; then
    warn "an ESP32-P4 is connected but not running the display firmware yet: run ./flash.sh"
else
    say "plug the board's middle USB-C port into this computer to start the display"
fi

echo
say "installed."
$needs_reboot && warn "fix the above and reboot (enrolling the key if the MOK screen appears) before plugging in the board"
cat <<EOF
    status:   systemctl --user status p4-usb-display
    log:      journalctl --user -u p4-usb-display -f
    remove:   ./install.sh --uninstall
EOF
