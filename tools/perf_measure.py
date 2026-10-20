#!/usr/bin/env python3
"""Measure c606-oss UI latency with the firmware's `perf` counter.

    python perf_measure.py /dev/ttyACM0 --swipes 10 --label baseline

Drives the device through the developer console, so it needs the same
permission as any other serial access. Prints one line per measurement plus
a compact summary that can be pasted into a commit message.
"""
import argparse
import re
import statistics
import sys
import time

import serial


def open_port(path):
    s = serial.serial_for_url(path, baudrate=115200, timeout=0.3, do_not_open=True)
    try:
        # deassert both before opening: this board otherwise latches the
        # USB-Serial-JTAG ROM loader (device stops running the app)
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    s.open()
    return s


def read_for(s, secs):
    out = b""
    end = time.time() + secs
    while time.time() < end:
        try:
            out += s.read(4096)
        except Exception:
            pass
    return out.decode("utf-8", "replace")


def command(s, cmd, wait=0.6):
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    return read_for(s, wait)


def parse_perf(text):
    m = re.search(
        r"perf up=(\d+)ms frames=(\d+) fps=([\d.]+) render_avg=([\d.]+) render_max=([\d.]+) "
        r"flush_avg=([\d.]+) flush_max=([\d.]+) press_n=(\d+) press_avg=([\d.]+) press_max=([\d.]+)",
        text)
    if not m:
        return None
    keys = ["up_ms", "frames", "fps", "render_avg", "render_max",
            "flush_avg", "flush_max", "press_n", "press_avg", "press_max"]
    return {k: float(v) if "." in v else int(v) for k, v in zip(keys, m.groups())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--mode", choices=("swipe", "key"), default="swipe")
    ap.add_argument("--swipes", type=int, default=10)
    ap.add_argument("--gap", type=float, default=0.35)
    ap.add_argument("--label", default="run")
    a = ap.parse_args()

    s = open_port(a.port)
    try:
        command(s, "perf reset")
        # alternate left/right swipes: each one changes page and repaints
        for i in range(a.swipes):
            if a.mode == "key":
                command(s, "key 0 1", 0.2)
            elif i % 2 == 0:
                command(s, "swipe 200 160 40 160 120", 0.25)
            else:
                command(s, "swipe 40 160 200 160 120", 0.25)
            time.sleep(a.gap)
        text = command(s, "perf", 0.8)
    finally:
        s.close()

    p = parse_perf(text)
    if not p:
        print(text[-600:])
        sys.exit("perf line not found")
    print(f"{a.label} [{a.mode}]: frames={p['frames']} fps={p['fps']} "
          f"render avg/max={p['render_avg']}/{p['render_max']} ms "
          f"flush avg/max={p['flush_avg']}/{p['flush_max']} ms "
          f"press n={p['press_n']} avg/max={p['press_avg']}/{p['press_max']} ms")


if __name__ == "__main__":
    main()
