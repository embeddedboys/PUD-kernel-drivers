# SPDX-License-Identifier: GPL-2.0-only
"""Band splitting must stay within the device-reported transfer limit.

ORACLE: SPEC
SOURCE: notes/usb-protocol.md ("传输上限" / PUD_CMD_GET_CAPS),
        notes/display-and-refresh.md ("分带")
EXPECTED:
    band_pixels(frame_max) = (min(USB_TRANS_MAX_SIZE, frame_max) - 12 - 16) / 3
    frame_max 65536 -> 21835 px   (RP2350, real-machine log)
    frame_max 32768 -> 10913 px   (RP2040, half-size transfers)
    worst case: 12 + 3 * band_pixels + 16 <= min(USB_TRANS_MAX_SIZE, frame_max)

The last relation is the invariant the driver's banding exists to protect; it is
derived from the formula, not from a measurement.
"""

from common import PASS, FAIL, INCONCLUSIVE, ERROR, tool_json, result, decide

TEST = "test_band_split"
KERNEL_REQUIRED = False
ORACLE = {
    "type": "SPEC",
    "source": "notes/usb-protocol.md; notes/display-and-refresh.md",
    "expected": "band formula and the two documented frame_max cases",
}

USB_MAX = 65535          # USB_TRANS_MAX_SIZE in pud.h
HEADER = 12              # PUD_EP1_HEADER_SIZE
FRAMING = 16             # QOI header + end marker
BPP_WORST = 3

CASES = [
    # (frame_max, expected band pixels, why)
    (65536, 21835, "RP2350 reports 65536, truncated to 65535"),
    (65535, 21835, "host ceiling"),
    (32768, 10913, "RP2040 accepts half-size transfers"),
]


def run():
    checks = []
    for frame_max, want, why in CASES:
        try:
            data = tool_json("protoctl", "band", "--frame-max", str(frame_max),
                             "--usb-max", str(USB_MAX))
        except (EnvironmentError, ValueError) as exc:
            return result(TEST, INCONCLUSIVE, ORACLE, [], [str(exc)])
        got = data["observation"]["band_pixels"]
        checks.append({"name": f"band({frame_max}) -> {want} ({why})",
                       "ok": got == want, "observed": got, "expected": want})
        # Invariant: a worst-case band still fits one transfer.
        bound = min(USB_MAX, frame_max)
        used = HEADER + BPP_WORST * got + FRAMING
        checks.append({"name": f"worst-case band fits frame_max={frame_max}",
                       "ok": used <= bound, "observed": used,
                       "expected": f"<= {bound}"})

    # Monotonic non-decreasing in the device limit.
    prev = -1
    for frame_max in (4096, 16384, 32768, 65536):
        data = tool_json("protoctl", "band", "--frame-max", str(frame_max),
                         "--usb-max", str(USB_MAX))
        got = data["observation"]["band_pixels"]
        ok = got >= prev
        checks.append({"name": f"monotonic at frame_max={frame_max}", "ok": ok,
                       "observed": got, "expected": f">= {prev}"})
        prev = got

    return result(TEST, decide(checks), ORACLE, checks)
