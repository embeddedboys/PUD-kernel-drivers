# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual USB frame preparation with exact-size input under ASan.

ORACLE: INVARIANT
SOURCE: notes/usb-protocol.md -- header plus padded payload must fit both the
reported transfer limit and DMA allocation, without reading beyond input.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from common import ERROR, FAIL, PASS, ROOT, result

TEST = "test_frame_prepare"
KERNEL_REQUIRED = False
ORACLE = {
    "type": "INVARIANT", "source": "notes/usb-protocol.md",
    "expected": "bounded payload, correct header, zero padding without over-read",
}


def verify():
    root = Path(ROOT)
    header = (root / "pud.h").read_text()
    usb = (root / "usb.c").read_text()
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        start = header.index("#define PUD_EP1_HEADER_SIZE")
        end = header.index("\n};", start) + len("\n};")
        (directory / "frame_header.h").write_text(header[start:end])
        start = header.index("static inline size_t pud_payload_capacity")
        end = header.index("\n}", start) + len("\n}")
        capacity = header[start:end]
        start = usb.index("static int pud_prepare_frame")
        end = usb.index("\nssize_t pud_flush", start)
        (directory / "frame_prepare.c").write_text(capacity + "\n" + usb[start:end])
        shutil.copyfile(root / "tests/frame_prepare_harness.c", directory / "check.c")
        subprocess.run(
            ["cc", "-fsanitize=address", "-g", "-I", str(directory),
             str(directory / "check.c"), "-o", str(directory / "check")],
            check=True, capture_output=True, timeout=15,
        )
        subprocess.run(
            [str(directory / "check")], check=True, capture_output=True, timeout=10,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
        )


def run():
    if not shutil.which("cc"):
        return result(TEST, ERROR, ORACLE, [], ["C compiler unavailable"])
    try:
        verify()
    except subprocess.CalledProcessError as exc:
        message = exc.stderr.decode(errors="replace") if exc.stderr else str(exc)
        return result(TEST, FAIL, ORACLE, [], [message])
    except subprocess.TimeoutExpired as exc:
        return result(TEST, FAIL, ORACLE, [], [str(exc)])
    return result(TEST, PASS, ORACLE, [{
        "name": "frame bounds and padding", "ok": True,
        "observed": "actual frame-preparation code passed ASan checks",
        "expected": ORACLE["expected"],
    }])
