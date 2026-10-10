#!/usr/bin/env python3
"""Put files on the ESP32's inbox over its USB serial console.

    ./esp32/upload.py hello.qd                  # a program
    ./esp32/upload.py ../qdos-doom/doom         # an app: a whole folder
    ./esp32/upload.py --list
    ./esp32/upload.py --remove hello.qd         # or an app's folder name

The other end is esp32/main/upload.c. The shell reads the inbox again as soon
as a file lands, as it does when the Pi's USB drive is handed back. Nothing
else may have the port open meanwhile, a monitor included. Needs pyserial.
"""

import argparse
import os
import re
import sys
import time
import zlib

import serial

STX = b"\x02"
REPLY = re.compile(rb"QDUP (.*)")


class Board:
    def __init__(self, port):
        self.port = serial.Serial()
        self.port.port = port
        self.port.baudrate = 115200
        self.port.timeout = 0.1
        # DTR up and RTS down while opening: the USB serial/JTAG unit resets
        # the chip when RTS is up with DTR down, which is how a port opened the
        # usual way leaves it
        self.port.dtr = True
        self.port.rts = False
        self.port.open()
        self.port.dtr = False
        self.pending = b""

    def close(self):
        self.port.close()

    def reply(self, timeout):
        """The next QDUP line, skipping the log around it; None on timeout."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                found = REPLY.search(line)
                if found:
                    return found.group(1).decode(errors="replace").strip()
            self.pending += self.port.read(4096)
        return None

    def command(self, line, timeout=5.0):
        self.port.write(STX + line.encode() + b"\n")
        return self.reply(timeout)

    def hello(self):
        # A port opened as it may have reset the chip; give it time to come up
        for _ in range(10):
            self.pending = b""
            if self.command("HELLO", timeout=1.0) == "OK":
                return True
        return False

    def put(self, data, path):
        crc = zlib.crc32(data) & 0xFFFFFFFF
        answer = self.command(f"PUT {len(data)} {crc:08x} {path}")
        if answer != "READY":
            return answer or "NO ANSWER"
        for i in range(0, len(data), 4096):
            self.port.write(data[i:i + 4096])
        # Wear-levelled flash writes at well under 100 KB/s
        return self.reply(10.0 + len(data) / 20000) or "NO ANSWER"


def files_of(arg):
    """(local file, inbox path) for a program or for each file of an app."""
    arg = arg.rstrip("/")
    if os.path.isdir(arg):
        app = os.path.basename(arg)
        return [(os.path.join(arg, name), f"{app}/{name}")
                for name in sorted(os.listdir(arg))
                if os.path.isfile(os.path.join(arg, name))]
    return [(arg, os.path.basename(arg))]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="*", help="programs or app folders")
    parser.add_argument("--port", default=os.environ.get("QDOS_PORT", "/dev/ttyACM0"))
    parser.add_argument("--list", action="store_true", help="what is on the inbox")
    parser.add_argument("--remove", metavar="NAME", action="append", default=[],
                        help="a program, or an app's folder")
    args = parser.parse_args()
    if not (args.paths or args.list or args.remove):
        parser.error("nothing to do")

    board = Board(args.port)
    try:
        if not board.hello():
            sys.exit(f"{args.port}: QDOS is not answering")

        failed = False
        for name in args.remove:
            answer = board.command(f"DEL {name}")
            print(f"remove {name}: {answer}")
            failed |= answer != "OK"

        for arg in args.paths:
            for local, path in files_of(arg):
                with open(local, "rb") as f:
                    data = f.read()
                answer = board.put(data, path)
                print(f"{path}: {len(data)} bytes, {answer}")
                failed |= answer != "OK"

        if args.list:
            board.port.write(STX + b"LS\n")
            while (answer := board.reply(5.0)) not in ("OK", None):
                if answer.startswith("F "):
                    size, path = answer[2:].split(" ", 1)
                    print(f"{int(size):>9}  {path}")
        sys.exit(1 if failed else 0)
    finally:
        board.close()


if __name__ == "__main__":
    main()
