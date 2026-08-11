#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import annotations

import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
CORE = Path(os.environ.get("KILIX_PDF_CORE", ROOT / "build/kilix-pdf-core"))
PDF = ROOT / "tests/minimal.pdf"
SYNC_END = b"\x1b[?2026l"


def read_until(master: int, process: subprocess.Popen[bytes], predicate) -> bytes:
    deadline = time.monotonic() + 8.0
    captured = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                chunk = os.read(master, 65536)
            except OSError as error:
                if error.errno == errno.EIO and process.poll() is not None:
                    break
                raise
            if chunk:
                captured.extend(chunk)
                if len(captured) > 16 * 1024 * 1024:
                    raise AssertionError("terminal protocol capture exceeded 16 MiB")
                if predicate(captured):
                    return bytes(captured)
        if process.poll() is not None:
            break
    raise AssertionError(
        f"protocol condition not reached; exit={process.poll()} bytes={len(captured)}"
    )


def run_viewer(kilix_rendering: bool) -> None:
    master, slave = pty.openpty()
    process: subprocess.Popen[bytes] | None = None
    try:
        window = struct.pack("HHHH", 24, 80, 640, 480)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, window)
        environment = os.environ.copy()
        if kilix_rendering:
            environment["KITTY_KILIX_RENDERING"] = "1"
        else:
            environment.pop("KITTY_KILIX_RENDERING", None)
        environment["KILIX_PDF_TEST_TERMINAL"] = "1"
        process = subprocess.Popen(
            [str(CORE), str(PDF)],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
            env=environment,
        )
        os.close(slave)
        slave = -1

        initial = read_until(master, process, lambda data: SYNC_END in data)
        if b"a=T" not in initial:
            raise AssertionError("initial frame was not transmitted")

        os.write(master, b"w")
        fitted = read_until(master, process, lambda data: SYNC_END in data)
        if b"a=T" not in fitted:
            raise AssertionError("fit-width change was not a full frame")

        os.write(master, b"j")
        if kilix_rendering:
            scrolled = read_until(
                master,
                process,
                lambda data: b"a=c" in data
                and b"C=1,N=2,q=2" in data
                and b"a=f" in data
                and SYNC_END in data,
            )
            if b"C=1,N=2,q=2" not in scrolled or b"a=f" not in scrolled:
                raise AssertionError("scroll did not compose and patch atomically")
        else:
            scrolled = read_until(master, process, lambda data: SYNC_END in data)
            if b"a=T" not in scrolled or b"a=c" in scrolled:
                raise AssertionError("standard terminal did not get a full frame")

        os.write(master, b"q")
        return_code = process.wait(timeout=5.0)
        if return_code != 0:
            raise AssertionError(f"interactive viewer exited {return_code}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
        if slave >= 0:
            os.close(slave)
        os.close(master)


def main() -> int:
    run_viewer(kilix_rendering=True)
    run_viewer(kilix_rendering=False)
    print("retained-scroll and standard-terminal fallback protocols passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
