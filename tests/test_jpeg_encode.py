# SPDX-License-Identifier: GPL-2.0-only
"""Check real JPEG encoding under ASan and decode independently with Pillow.

ORACLE: REQUIREMENT
SOURCE: notes/encoders.md -- aligned images decode; bounded failures return
negative errno with no partial output. Only kernel types/allocation are shimmed.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from common import ROOT, ERROR, FAIL, PASS, result

TEST = "test_jpeg_encode"
KERNEL_REQUIRED = False
ORACLE = {
    "type": "REQUIREMENT",
    "source": "notes/encoders.md",
    "expected": "valid JPEG round trip and explicit bounded failures",
}
REPOSITORY = Path(ROOT)


def prepare_sources(directory):
    (directory / "linux").mkdir()
    headers = "\n".join(
        f"#include <{name}>" for name in ("stdint.h", "string.h", "stdlib.h", "limits.h")
    ) + "\n"
    for name in ("init.h", "module.h", "kernel.h", "limits.h"):
        (directory / "linux" / name).write_text(headers)
    for name in ("jpegenc.h", "jpegenc.c"):
        shutil.copyfile(REPOSITORY / name, directory / name)

    encoder = (REPOSITORY / "encoder.c").read_text()
    start = encoder.index("#define PUD_JPEG_MCU_SIZE")
    end = encoder.index("\nint qoi_encode_rgb565", start)
    (directory / "jpeg_wrapper.c").write_text(encoder[start:end])
    shutil.copyfile(REPOSITORY / "tests/jpeg_encode_harness.c", directory / "check.c")


def check_decoded_images(directory):
    from PIL import Image

    for pattern in (0, 1):
        with Image.open(directory / f"{pattern}.jpg") as image:
            image.load()
            assert image.size == (800, 480), image.size
            if pattern == 0:
                red, green, blue = image.getpixel((400, 240))
                # A lossy red image must remain visibly red; exact bytes are
                # intentionally not used as a golden for JPEG quantisation.
                assert red > 220 and green < 30 and blue < 30


def verify():
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        prepare_sources(directory)
        subprocess.run(
            ["cc", "-fsanitize=address", "-g", "-I", str(directory),
             str(directory / "check.c"), str(directory / "jpegenc.c"),
             "-o", str(directory / "check")],
            check=True, capture_output=True, timeout=15,
        )
        subprocess.run(
            [str(directory / "check"), str(directory)],
            check=True, capture_output=True, timeout=10,
            # Restricted hosts may run under ptrace: leak checks are unavailable,
            # but ASan still checks reads and writes against allocation bounds.
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
        )
        check_decoded_images(directory)


def run():
    if not shutil.which("cc"):
        return result(TEST, ERROR, ORACLE, [], ["C compiler unavailable"])
    try:
        import PIL.Image
    except ImportError:
        return result(TEST, ERROR, ORACLE, [], ["Pillow unavailable"])
    try:
        verify()
    except subprocess.CalledProcessError as exc:
        diagnostic = exc.stderr.decode(errors="replace") if exc.stderr else str(exc)
        return result(TEST, FAIL, ORACLE, [], [diagnostic])
    except (subprocess.TimeoutExpired, AssertionError) as exc:
        return result(TEST, FAIL, ORACLE, [], [str(exc)])
    return result(TEST, PASS, ORACLE, [{
        "name": "JPEG round trip and buffer bounds", "ok": True,
        "observed": "ASan and Pillow checks passed", "expected": ORACLE["expected"],
    }])
