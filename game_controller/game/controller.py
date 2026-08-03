"""Voice/gesture controller: reads BLE-relayed commands from the DK over serial.

The game_controller dongle sends direction commands over BLE (Nordic UART
Service); the game_receiver app on the DK acts as the BLE central and relays
each one onto its console UART as a plain-text line, notably:

    Command: UP
    Command: DOWN
    Command: LEFT
    Command: RIGHT

This module opens the serial port in a background thread, parses those lines and
pushes the corresponding direction onto a thread-safe queue that the game loop
drains. Non-direction commands (and all other status lines) are ignored.
"""

from __future__ import annotations

import queue
import re
import threading

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - import guard
    raise SystemExit(
        "pyserial is required for serial input. Install it with:\n"
        "    pip install pyserial"
    ) from exc

# Directions are (dx, dy) with y growing downwards, matching screen coordinates.
DIRECTIONS = {
    "UP": (0, -1),
    "DOWN": (0, 1),
    "LEFT": (-1, 0),
    "RIGHT": (1, 0),
}

_KEYWORD_RE = re.compile(r"Command:\s*(\w+)")

# USB vendor IDs of the debug probes used on Nordic DKs (SEGGER J-Link OB,
# Nordic Semiconductor), used to pick out candidate ports.
_KNOWN_VIDS = {0x1366, 0x1915}

# The J-Link OB exposes two CDC VCOMs. The firmware console is the second one,
# at USB interface 2 (/dev/serial/by-id/...-if02). USB interface numbers are
# stable across reboots and replugs, unlike the /dev/ttyACMx ordering, so we
# select on the interface rather than probing traffic.
_CONSOLE_USB_INTERFACE = 2


def list_candidate_ports() -> list[str]:
    """Return likely DK serial ports, best guesses first.

    Scores each enumerated port by USB vendor ID and description so that the
    debug-probe VCOMs sort ahead of unrelated serial devices.
    """

    def score(p) -> int:
        s = 0
        if p.vid in _KNOWN_VIDS:
            s += 10
        desc = f"{p.description} {p.product or ''}".lower()
        if "j-link" in desc or "nordic" in desc:
            s += 5
        if "ttyacm" in p.device.lower() or "usbmodem" in p.device.lower():
            s += 2
        return s

    ranked = sorted(list_ports.comports(), key=score, reverse=True)
    return [p.device for p in ranked if score(p) > 0]


def find_controller_port() -> str | None:
    """Return the firmware console port, selected by stable USB interface.

    The J-Link's console VCOM is always at USB interface @ref
    _CONSOLE_USB_INTERFACE, regardless of how Linux numbers the ttyACMx
    devices. Falls back to the highest-ranked candidate if that interface
    cannot be identified (e.g. a board that does not report a location).

    @return Device path, or None if no candidate ports were found.
    """
    # location looks like '5-1:1.2'; the trailing '.N' is the USB interface.
    suffix = f":1.{_CONSOLE_USB_INTERFACE}"
    for p in list_ports.comports():
        if p.vid in _KNOWN_VIDS and p.location and p.location.endswith(suffix):
            return p.device

    candidates = list_candidate_ports()
    return candidates[0] if candidates else None


class SerialController:
    """Background reader that turns serial keyword events into directions."""

    def __init__(self, port: str | None = None, baudrate: int = 115200):
        # port=None auto-detects the firmware port at start().
        self.port = port
        self.baudrate = baudrate
        self.directions: "queue.Queue[tuple[int, int]]" = queue.Queue()
        self._serial: serial.Serial | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()

    def start(self) -> None:
        """Open the port (auto-detecting if needed) and read in a daemon thread."""
        if self.port is None:
            self.port = find_controller_port()
            if self.port is None:
                raise RuntimeError(
                    "No DK serial port found. Connect the board, or pass a port "
                    "explicitly (e.g. SerialController('/dev/ttyACM1'))."
                )
        print(f"[controller] using serial port {self.port}")
        self._serial = serial.Serial(self.port, self.baudrate, timeout=0.5)
        self._thread = threading.Thread(target=self._run, name="serial-reader", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        if self._serial is not None:
            self._serial.close()

    def _run(self) -> None:
        assert self._serial is not None
        while not self._stop.is_set():
            try:
                line = self._serial.readline().decode("utf-8", errors="replace").strip()
            except (serial.SerialException, OSError):
                break
            if not line:
                continue
            match = _KEYWORD_RE.search(line)
            if not match:
                continue
            direction = DIRECTIONS.get(match.group(1))
            if direction is not None:
                self.directions.put(direction)

if __name__ == "__main__":
    import sys

    port = sys.argv[1] if len(sys.argv) > 1 else None  # None => auto-detect
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    # Reverse map so we can print a readable name, not just the (dx, dy) tuple.
    NAMES = {v: k for k, v in DIRECTIONS.items()}

    controller = SerialController(port, baud)
    controller.start()
    print(f"Listening on {controller.port} @ {baud} (Ctrl-C to stop)...")
    try:
        while True:
            direction = controller.directions.get()  # blocks until one arrives
            print(f"{NAMES[direction]:<5} {direction}")
    except KeyboardInterrupt:
        pass
    finally:
        controller.stop()