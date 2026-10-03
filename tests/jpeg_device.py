# SPDX-License-Identifier: GPL-2.0-only
"""Hardware observations shared by the QEMU JPEG tests, without verdicts."""

from contextlib import contextmanager
from dataclasses import dataclass
import json
import mmap
from pathlib import Path
import re
import struct
import subprocess
import time

import usb.core
import usb.util

ROOT = Path(__file__).resolve().parents[1]
POLL_SECONDS = 0.2
READY_SECONDS = 3
USB_TIMEOUT_MS = 500
LOG_INTERFACE = 1
LOG_ENDPOINT = 0x83
REQ_LOG = 0x82
REQ_RUN = 0x83
REQ_LOG_STATS = 0x84
COUNTERS_PATTERN = re.compile(
    rb"submitted (\d+) drawn (\d+) dropped (\d+) decode_failed (\d+)"
)


@dataclass(frozen=True)
class DecoderCounters:
    submitted: int
    drawn: int
    dropped: int
    decode_failed: int


class DeviceLog:
    def __init__(self):
        self.device = usb.core.find(idVendor=0x33C3, idProduct=0x7788)
        if self.device is None:
            raise EnvironmentError("ZX USB device is unavailable")
        # Do not set configuration: QEMU's kernel driver already owns interface 0.

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        usb.util.dispose_resources(self.device)

    def _request(self, request, value=0, data=None):
        self.device.ctrl_transfer(
            0x40, request, value, LOG_INTERFACE, data, timeout=USB_TIMEOUT_MS
        )

    def _read_log(self, first_sequence):
        self._request(REQ_LOG, first_sequence)
        output = bytearray()
        while True:
            try:
                chunk = self.device.read(LOG_ENDPOINT, 4096, timeout=USB_TIMEOUT_MS)
            except usb.core.USBTimeoutError:
                # Missing terminators can occur on emulated xHCI. The caller
                # accepts only a complete counter line, otherwise polls again.
                return output
            if not chunk:
                return output
            output.extend(chunk)

    def counters(self):
        self._request(REQ_LOG_STATS)
        response = bytes(self.device.read(LOG_ENDPOINT, 64, timeout=USB_TIMEOUT_MS))
        if len(response) < 16:
            raise EnvironmentError("short device log-state response")
        _, _, sequence, _ = struct.unpack("<IIII", response[:16])
        if sequence >= 65534:
            raise EnvironmentError("log sequence exceeds the request field")

        self._request(REQ_RUN, data=b"zxdisp_stats\0")
        deadline = time.monotonic() + READY_SECONDS
        while time.monotonic() < deadline:
            matches = COUNTERS_PATTERN.findall(self._read_log(sequence + 1))
            if matches:
                return DecoderCounters(*map(int, matches[-1]))
            time.sleep(POLL_SECONDS)
        raise TimeoutError("fresh device decoder counters unavailable")

    def wait_idle(self):
        deadline = time.monotonic() + READY_SECONDS
        previous = self.counters()
        while time.monotonic() < deadline:
            time.sleep(POLL_SECONDS)
            current = self.counters()
            if current == previous:
                return current
            previous = current
        raise TimeoutError("display did not become idle")


def framebuffer_info():
    nodes = [
        node for node in Path("/sys/class/graphics").glob("fb[0-9]*")
        if (node / "name").read_text().strip() == "pud-drmdrmfb"
    ]
    if len(nodes) != 1:
        raise EnvironmentError("expected exactly one PUD framebuffer")
    response = subprocess.check_output(
        [str(ROOT / "tools/fbctl"), "info", "--fd", nodes[0].name[2:], "--json"],
        text=True, timeout=READY_SECONDS,
    )
    return Path("/dev") / nodes[0].name, json.loads(response)["observation"]


@contextmanager
def paused_framebuffer_console():
    bindings = []
    try:
        for console in Path("/sys/class/vtconsole").glob("vtcon*"):
            if "frame buffer" not in (console / "name").read_text():
                continue
            path = console / "bind"
            bindings.append((path, path.read_text()))
            path.write_text("0")
        yield
    finally:
        for path, value in bindings:
            path.write_text(value)


def pack_image(image, info):
    if image.size != (info["xres"], info["yres"]):
        raise ValueError("image dimensions do not match the framebuffer")
    bytes_per_pixel = info["bits_per_pixel"] // 8
    if bytes_per_pixel not in (2, 4):
        raise EnvironmentError("unsupported framebuffer pixel format")
    pixel_format = "<H" if bytes_per_pixel == 2 else "<I"
    payload = bytearray(info["line_length"] * info["yres"])
    for index, channels in enumerate(image.convert("RGB").getdata()):
        pixel = 0
        for name, channel in zip(("red", "green", "blue"), channels):
            field = info[name]
            scaled = (channel * ((1 << field["length"]) - 1) + 127) // 255
            pixel |= scaled << field["offset"]
        y, x = divmod(index, info["xres"])
        struct.pack_into(pixel_format, payload,
                         y * info["line_length"] + x * bytes_per_pixel, pixel)
    return payload


@contextmanager
def mapped_framebuffer(path, info):
    with path.open("r+b", buffering=0) as file:
        with mmap.mmap(file.fileno(), info["smem_len"]) as memory:
            yield memory


def kernel_log():
    return subprocess.check_output(["dmesg"], text=True, timeout=READY_SECONDS)
