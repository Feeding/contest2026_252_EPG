#!/usr/bin/env python3
"""Flash a BK7258 without having to catch a 10-second reset window.

The CH340 on this board has its RTS#/CTS# pins unconnected (see the board
schematic), so nothing can pulse the chip's CEN pin: the only way to reset is
the RST button.  ``bk_loader`` waits about ten seconds for that and then gives
up, which turns flashing into a game of pressing RST at the right moment.

This script splits the job.  It hammers CMD_LinkCheck with no timeout until the
chip answers -- so a single RST press at any moment is enough -- and only then
hands the already-open session to ``bk_loader``, which does the actual erase
and write.  The chip stays in download mode once it has answered, so the
handover is safe.

Usage:
    bk_flash.py <image> <start_addr_hex> [port]
    bk_flash.py --read <outfile> <start_hex> <len_hex> [port]
"""
import os
import subprocess
import sys
import time

import serial

LINK = b"\x01\xe0\xfc\x01\x00"
RESP = b"\x04\x0e"
LINK_BAUDS = [115200, 1500000]
XFER_BAUD = 1500000
DEFAULT_PORT = "/dev/cu.usbserial-310"


def wait_for_chip(port, baud=115200, announce_every=2000):
    """Block until the chip answers a link check.

    The l_bootloader download window is only tens of milliseconds wide, so this
    must not leave gaps: the port is opened once and the link check goes out
    back to back until something answers.  An earlier version cycled baud rates
    and reopened the port each round, and lost RST presses in the seams.
    """
    probes = 0
    print(f"waiting for the chip at {baud} baud -- press RST once, any time",
          flush=True)

    while not os.path.exists(port):
        time.sleep(0.5)

    ser = serial.Serial(port, baud, timeout=0)
    try:
        # Never drive DTR/RTS: on this board they are not wired to CEN, and
        # asserting them only confuses the CH340.
        ser.dtr = False
        ser.rts = False
        ser.reset_input_buffer()

        while True:
            ser.write(LINK)
            probes += 1
            data = ser.read(64)
            if data and RESP in data:
                print(f"chip is in download mode ({probes} probes)", flush=True)
                return baud
            if probes % announce_every == 0:
                print(f"  still waiting ({probes} probes)...", flush=True)
    finally:
        try:
            ser.close()
        except Exception:
            pass


def run_bk_loader(args):
    """Hand over to bk_loader, which owns the real flash protocol."""
    cmd = ["bk_loader"] + args
    print("-> " + " ".join(cmd), flush=True)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    text = out.stdout + out.stderr
    for line in text.splitlines():
        if any(k in line for k in ("Get bus", "Download complete",
                                   "Read complete", "fail", "Elapse",
                                   "flash mid")):
            print("   " + line.strip(), flush=True)
    return "all pass" in text


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--read":
        out_file, start, length = sys.argv[2], sys.argv[3], sys.argv[4]
        port = sys.argv[5] if len(sys.argv) > 5 else DEFAULT_PORT
        wait_for_chip(port)
        ok = run_bk_loader([
            "read", "-p", port, "-b", str(XFER_BAUD), "--reset_type", "3",
            "-g", "300", "-f", f"{out_file}@{start}-{length}",
        ])
    else:
        image = sys.argv[1]
        start = sys.argv[2]
        port = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_PORT
        print(f"image : {image}  {os.path.getsize(image)} bytes")
        print(f"target: {start}")
        wait_for_chip(port)
        ok = run_bk_loader([
            "download", "-p", port, "-b", str(XFER_BAUD), "--reset_type", "3",
            "-g", "300", "-e", "1", "-r", "-i", os.path.abspath(image),
            "-s", start,
        ])

    print("OK" if ok else "FAILED", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
