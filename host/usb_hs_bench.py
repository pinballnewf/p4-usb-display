#!/usr/bin/env python3
"""Bulk-OUT throughput benchmark for the P4 USB HS test firmware.

Usage: python3 usb_hs_bench.py [seconds] [chunk_kib]
"""
import sys
import time
import struct

import usb.core
import usb.util

VID, PID = 0x303A, 0x4020
EP_OUT = 0x01
REQ_GET_RX_TOTAL = 0x01

SPEEDS = {1: "low (1.5 Mbit/s)", 2: "full (12 Mbit/s)", 3: "high (480 Mbit/s)", 4: "super (5 Gbit/s)"}


def rx_total(dev):
    """Vendor IN control request to interface 0: device's running bulk OUT byte count."""
    data = dev.ctrl_transfer(0xC1, REQ_GET_RX_TOTAL, 0, 0, 8, timeout=1000)
    return struct.unpack("<Q", bytes(data))[0]


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    chunk = int(sys.argv[2]) * 1024 if len(sys.argv) > 2 else 64 * 1024

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit(f"device {VID:04x}:{PID:04x} not found - is the middle port plugged in?")
    print(f"found {dev.product!r}, bus speed: {SPEEDS.get(dev.speed, dev.speed)}")

    dev.set_configuration()
    usb.util.claim_interface(dev, 0)

    start_total = rx_total(dev)

    payload = bytes(range(256)) * (chunk // 256)
    sent = 0
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        sent += dev.write(EP_OUT, payload, timeout=2000)
    elapsed = time.perf_counter() - t0

    end_total = rx_total(dev)
    received = end_total - start_total

    mbps = sent / elapsed / 1e6
    print(f"sent {sent / 1e6:.1f} MB in {elapsed:.2f}s = {mbps:.2f} MB/s ({mbps * 8:.0f} Mbit/s)")
    print(f"device counted {received / 1e6:.1f} MB ({'OK' if received == sent else 'MISMATCH'})")
    fps = sent / elapsed / (800 * 1280 * 2)
    print(f"=> {fps:.1f} fps of raw full-frame 800x1280 RGB565")


if __name__ == "__main__":
    main()
