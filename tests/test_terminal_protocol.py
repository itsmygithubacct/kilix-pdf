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
READY = b"\x1b]1337;SetUserVar=kilix_pdf_ready=djE=\x07"


def graphics_controls(data: bytes) -> list[bytes]:
    controls: list[bytes] = []
    offset = 0
    while True:
        start = data.find(b"\x1b_G", offset)
        if start < 0:
            return controls
        separator = data.find(b";", start + 3)
        terminator = data.find(b"\x1b\\", separator + 1)
        if separator < 0 or terminator < 0:
            return controls
        controls.append(data[start + 3 : separator])
        offset = terminator + 2


def control_parameters(control: bytes) -> dict[bytes, bytes]:
    parameters: dict[bytes, bytes] = {}
    for field in control.split(b","):
        key, separator, value = field.partition(b"=")
        if separator:
            parameters[key] = value
    return parameters


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
        environment["KILIX_PDF_READINESS"] = "1"
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

        initial = read_until(master, process, lambda data: READY in data)
        if initial.index(READY) < initial.index(SYNC_END):
            raise AssertionError("readiness preceded the completed first frame")
        if b"a=T" not in initial:
            raise AssertionError("initial frame was not transmitted")
        transmit = next(
            control
            for control in graphics_controls(initial)
            if control.startswith(b"a=T,")
        )
        if int(control_parameters(transmit).get(b"z", b"0")) >= -1073741824:
            raise AssertionError("framebuffer image obscures terminal pointer layer")

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
            controls = graphics_controls(scrolled)
            compose = next(
                (control for control in controls if control.startswith(b"a=c,")),
                None,
            )
            if compose is None:
                raise AssertionError("scroll compose control was not captured")
            compose_parameters = control_parameters(compose)
            destination_y = int(compose_parameters[b"y"])
            source_y = int(compose_parameters[b"Y"])
            if destination_y <= 0 or source_y <= destination_y:
                raise AssertionError("scroll was not clipped below fixed chrome")
            first_chunks = [
                control
                for control in controls
                if control.startswith(b"a=f,") and b"r=1" in control
            ]
            if not first_chunks:
                raise AssertionError("scroll did not patch its exposed strip")
            if any(int(control_parameters(control)[b"y"]) < destination_y
                   for control in first_chunks):
                raise AssertionError("scroll patch crossed into fixed chrome")
            continuations = [
                control
                for control in controls
                if control.startswith(b"a=f,q=") and b"m=" in control
            ]
            if any(b",i=" in control for control in continuations):
                raise AssertionError("animation-frame continuation repeated image id")

            os.write(master, b"k")
            reverse = read_until(
                master,
                process,
                lambda data: b"a=c" in data and b"a=f" in data and SYNC_END in data,
            )
            reverse_controls = graphics_controls(reverse)
            reverse_compose = next(
                control
                for control in reverse_controls
                if control.startswith(b"a=c,")
            )
            reverse_parameters = control_parameters(reverse_compose)
            reverse_source_y = int(reverse_parameters[b"Y"])
            reverse_destination_y = int(reverse_parameters[b"y"])
            if reverse_source_y <= 0 or reverse_destination_y <= reverse_source_y:
                raise AssertionError("reverse scroll was not clipped below fixed chrome")
            reverse_patches = [
                control
                for control in reverse_controls
                if control.startswith(b"a=f,") and b"r=1" in control
            ]
            if not reverse_patches or any(
                int(control_parameters(control)[b"y"]) < reverse_source_y
                for control in reverse_patches
            ):
                raise AssertionError("reverse scroll patch crossed into fixed chrome")
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
    failed = subprocess.run([str(CORE), str(ROOT / 'tests/absent.pdf')],
                            capture_output=True, timeout=5,
                            env=dict(os.environ, KILIX_PDF_READINESS='1'))
    if failed.returncode == 0 or READY in failed.stdout:
        raise AssertionError('a document-open failure announced readiness')
    print("retained-scroll and standard-terminal fallback protocols passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
