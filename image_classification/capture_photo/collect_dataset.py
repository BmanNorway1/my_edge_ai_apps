#!/usr/bin/env python3
"""Collect RGB565 frames streamed over SEGGER RTT into dataset folders.

The firmware sends raw RGB565 frames (CAM_WIDTH * CAM_HEIGHT * 2 bytes each).
This script reassembles them from the RTT byte stream and saves each frame
as a PNG (lossless, RGB888) ready for upload to Edge Impulse.

This ensures training data and inference data go through the exact same
camera pipeline — no JPEG ISP processing during training.

The firmware uses two RTT channels:
    channel 1  single shot (sw0)  -> single-capture folder
    channel 2  stream mode (sw1)  -> stream folder

Setup:
    pip install pylink-square pillow numpy

Usage:
    python3 collect_dataset.py
"""
import os
import time

import numpy as np
import pylink
from PIL import Image

# ---------------------------------------------------------------------------
# Settings -- edit these to match your setup.
# ---------------------------------------------------------------------------
SINGLE_OUTDIR = "/home/nib1/ncs/arducam_pictures/single_capture_mode"
STREAM_OUTDIR  = "/home/nib1/ncs/arducam_pictures/stream_mode"

DEVICE = "nRF54LM20A_M33"   # J-Link target device name
SPEED  = 4000               # SWD speed in kHz

SINGLE_CHANNEL = 1          # RTT channel for single-shot photos (sw0)
STREAM_CHANNEL = 2          # RTT channel for stream-mode photos (sw1)
PREFIX = "img_"             # output filename prefix

# Must match CAM_WIDTH / CAM_HEIGHT in your firmware exactly.
CAM_WIDTH  = 96
CAM_HEIGHT = 96

# Derived -- do not edit.
FRAME_BYTES = CAM_WIDTH * CAM_HEIGHT * 2   # 2 bytes per RGB565 pixel


def rgb565_to_png(raw_bytes: bytes, width: int, height: int, out_path: str) -> None:
    """Convert a raw RGB565 byte buffer to an RGB888 PNG file.

    Bit expansion uses the standard formula that maps the maximum value of
    each channel to exactly 255:
        5-bit channel:  value * 255 // 31
        6-bit channel:  value * 255 // 63

    This matches the LUT normalization used in the inference firmware
    (float)i / 31.f and (float)i / 63.f, so training and inference see
    numerically identical colour values.
    """
    # Interpret the buffer as an array of big-endian uint16 values.
    # The Arducam sends the high byte first (same as extract_pixel() in firmware).
    pixels = np.frombuffer(raw_bytes, dtype=np.dtype(">u2"))  # big-endian uint16

    r5 = (pixels >> 11) & 0x1F
    g6 = (pixels >>  5) & 0x3F
    b5 = (pixels >>  0) & 0x1F

    # Expand to 8-bit -- identical scaling to the inference LUTs.
    r8 = (r5 * 255 // 31).astype(np.uint8)
    g8 = (g6 * 255 // 63).astype(np.uint8)
    b8 = (b5 * 255 // 31).astype(np.uint8)

    rgb = np.stack([r8, g8, b8], axis=-1).reshape(height, width, 3)
    Image.fromarray(rgb, mode="RGB").save(out_path, format="PNG")


class ChannelCollector:

    def __init__(self, channel: int, outdir: str, prefix: str) -> None:
        self.channel = channel
        self.outdir  = outdir
        self.prefix  = prefix
        os.makedirs(outdir, exist_ok=True)

        # Resume numbering from the highest existing file.
        existing = [f for f in os.listdir(outdir)
                    if f.startswith(prefix) and f.endswith(".png")]
        self.count = 0
        for f in existing:
            try:
                n = int(f[len(prefix):-len(".png")])
            except ValueError:
                continue
            self.count = max(self.count, n)
        if existing:
            print(f"[ch{channel}] {len(existing)} existing image(s) in {outdir}/, "
                  f"continuing from {self.count + 1}")

        self.buf = bytearray()

    def feed(self, chunk: bytes) -> None:
        """Accumulate bytes and flush complete frames."""
        self.buf.extend(chunk)

        # Every FRAME_BYTES of data is one complete RGB565 frame.
        while len(self.buf) >= FRAME_BYTES:
            frame_bytes = bytes(self.buf[:FRAME_BYTES])
            del self.buf[:FRAME_BYTES]

            self.count += 1
            path = os.path.join(self.outdir, f"{self.prefix}{self.count:04d}.png")
            rgb565_to_png(frame_bytes, CAM_WIDTH, CAM_HEIGHT, path)
            print(f"[ch{self.channel}] saved {path} ({FRAME_BYTES} raw bytes -> PNG)")


def main() -> None:
    collectors = [
        ChannelCollector(SINGLE_CHANNEL, SINGLE_OUTDIR, PREFIX),
        ChannelCollector(STREAM_CHANNEL, STREAM_OUTDIR, PREFIX),
    ]

    jlink = pylink.JLink()
    jlink.open()
    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(DEVICE, speed=SPEED)
    jlink.rtt_start()
    print(f"Connected to {DEVICE}. Reading RTT channels "
          f"{SINGLE_CHANNEL} (single) and {STREAM_CHANNEL} (stream).")
    print(f"Expecting {FRAME_BYTES}-byte RGB565 frames ({CAM_WIDTH}x{CAM_HEIGHT}).")
    print("Press sw0 for a photo, sw1 for stream mode. Ctrl+C to stop.")

    total_start = sum(c.count for c in collectors)
    try:
        while True:
            got_data = False
            for c in collectors:
                chunk = jlink.rtt_read(c.channel, 4096)
                if chunk:
                    c.feed(bytes(chunk))
                    got_data = True
            if not got_data:
                time.sleep(0.01)
    except KeyboardInterrupt:
        saved = sum(c.count for c in collectors) - total_start
        print(f"\nstopping — {saved} new image(s) this session")
        for c in collectors:
            print(f"  ch{c.channel}: {c.count} total in {c.outdir}/")
    finally:
        try:
            jlink.rtt_stop()
        finally:
            jlink.close()


if __name__ == "__main__":
    main()
