# SPDX-License-Identifier: GPL-2.0-only
"""QEMU guest test for JPEG updates and capacity-failure recovery.

ORACLE: REQUIREMENT
SOURCE: notes/jpeg-validation.md
EXPECTED: valid frames draw once without new drops/decode failures; an
oversized frame is rejected before USB, and the following valid frame recovers.
"""

import argparse
from pathlib import Path
import re
import sys
import time

from PIL import Image
import usb.core

from jpeg_device import (
    DeviceLog, POLL_SECONDS, READY_SECONDS, framebuffer_info, kernel_log,
    mapped_framebuffer, pack_image, paused_framebuffer_console,
)


def prepare_frames(info, images=None):
    if images is None:
        size = (info["xres"], info["yres"])
        return [
            (name, pack_image(Image.new("RGB", size, color), info), False)
            for name, color in (("red", "#ff0000"), ("green", "#00ff00"),
                                ("blue", "#0000ff"))
        ]
    frames = []
    for name in ("gradient", "text-lines", "photo", "noise", "composite"):
        with Image.open(images / (name + ".png")) as image:
            frames.append((name, pack_image(image, info), name == "noise"))
    return frames


def verify_update(log, memory, name, payload, expect_rejection):
    before = log.counters()
    previous_log = kernel_log()
    memory.seek(0)
    memory.write(payload)
    deadline = time.monotonic() + READY_SECONDS

    while time.monotonic() < deadline:
        after = log.counters()
        new_log = kernel_log()[len(previous_log):]
        if expect_rejection and "band encode failed: -28" in new_log:
            break
        if not expect_rejection and after.drawn > before.drawn:
            break
        time.sleep(POLL_SECONDS)
    else:
        raise TimeoutError(f"{name}: expected update result did not arrive")

    print(f"{name}: {before} -> {after}")
    if expect_rejection:
        assert after == before, f"{name}: rejected frame reached the device"
        print("expected host capacity rejection: -ENOSPC")
        return
    assert after.submitted == before.submitted + 1, f"{name}: submission count"
    assert after.drawn == before.drawn + 1, f"{name}: draw count"
    assert after.dropped == before.dropped, f"{name}: new dropped frame"
    assert after.decode_failed == before.decode_failed, f"{name}: decoder error"
    assert not re.search(r"encode failed|EP1 transfer failed", new_log), new_log


def run(images):
    with DeviceLog() as log:
        path, info = framebuffer_info()
        frames = prepare_frames(info, images)
        print(f"USB speed {log.device.speed}, framebuffer {info['xres']}x{info['yres']}")
        with paused_framebuffer_console():
            log.wait_idle()
            with mapped_framebuffer(path, info) as memory:
                for name, payload, expect_rejection in frames:
                    verify_update(log, memory, name, payload, expect_rejection)
            assert not re.search(r"WARNING:|BUG:|Oops", kernel_log())
        print("JPEG_QEMU_DEVICE_PASS")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--images", type=Path, help="generated complex fixtures")
    args = parser.parse_args(argv)
    try:
        run(args.images)
    except (TimeoutError, usb.core.USBTimeoutError) as exc:
        print(f"TIMEOUT: {exc}", file=sys.stderr)
        return 4
    except (EnvironmentError, usb.core.USBError) as exc:
        print(f"ENVIRONMENT_ERROR: {exc}", file=sys.stderr)
        return 3
    except (AssertionError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
