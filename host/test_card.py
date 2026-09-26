#!/usr/bin/env python3
"""Stream an animated landscape test card to the P4 USB display.

Usage: test_card.py [--seconds N] [--fps N] [--rotate 90|270] [--quality Q]
"""
import argparse
import time

from PIL import Image, ImageDraw, ImageFont

from p4disp import P4Display, encode

W, H = 1280, 800  # landscape, as the desktop would see it


def load_font(size):
    for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def make_background():
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    bw = W // len(bars)
    for i, c in enumerate(bars):
        d.rectangle([i * bw, 0, (i + 1) * bw - 1, H // 2], fill=c)
    for x in range(W):  # grey ramp
        v = x * 255 // (W - 1)
        d.line([x, H // 2, x, H * 5 // 8], fill=(v, v, v))
    d.rectangle([0, 0, W - 1, H - 1], outline=(255, 255, 255), width=4)
    f = load_font(40)
    d.text((20, 16), "TOP LEFT", fill=(0, 0, 0), font=f)
    d.text((W - 260, H - 60), "BOTTOM RIGHT", fill=(255, 255, 255), font=load_font(28))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--fps", type=float, default=60, help="target send rate")
    ap.add_argument("--rotate", type=int, default=90, choices=(0, 90, 180, 270))
    ap.add_argument("--quality", type=int, default=85)
    args = ap.parse_args()

    disp = P4Display()
    print(f"connected, {'high' if disp.high_speed else 'FULL (slow!)'} speed")
    bg = make_background()
    big = load_font(72)

    t0 = time.perf_counter()
    period = 1.0 / args.fps
    n = 0
    total_bytes = 0
    enc_s = send_s = 0.0
    last_report = t0
    while (now := time.perf_counter()) - t0 < args.seconds:
        img = bg.copy()
        d = ImageDraw.Draw(img)
        x = int((now - t0) * 400) % (W - 80)
        d.rectangle([x, H * 5 // 8 + 10, x + 80, H - 10], fill=(255, 64, 0))
        d.text((40, H * 5 // 8 + 40), f"frame {n}", fill=(255, 255, 255), font=big)

        a = time.perf_counter()
        jpeg = encode(img, args.rotate, args.quality)
        b = time.perf_counter()
        disp.send_jpeg(jpeg)
        c = time.perf_counter()
        enc_s += b - a
        send_s += c - b
        total_bytes += len(jpeg)
        n += 1

        if c - last_report >= 2:
            s = disp.stats()
            el = c - t0
            print(f"sent {n / el:5.1f} fps  {total_bytes / n / 1024:5.0f} KB/frame  "
                  f"enc {enc_s / n * 1000:4.1f} ms  usb {send_s / n * 1000:4.1f} ms | board: shown "
                  f"{s.get('frames_shown')} stale {s['frames_stale']} decode "
                  f"{s.get('decode_us_avg', 0) / 1000:.1f} ms fail {s.get('decode_fail')} "
                  f"bad {s['bad_header']} trunc {s['truncated']}")
            last_report = c

        sleep = t0 + n * period - time.perf_counter()
        if sleep > 0:
            time.sleep(sleep)

    time.sleep(0.2)
    s = disp.stats()
    print(f"done: {n} frames sent, board received {s['frames_rx']}, shown {s.get('frames_shown')}, "
          f"stale-dropped {s['frames_stale']}, decode fails {s.get('decode_fail')}")


if __name__ == "__main__":
    main()
