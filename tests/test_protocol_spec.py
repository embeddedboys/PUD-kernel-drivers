# SPDX-License-Identifier: GPL-2.0-only
"""Protocol constants must match the authoritative definition.

ORACLE: SPEC
SOURCE: notes/usb-protocol.md (authoritative field definitions)
EXPECTED: the constants documented there, extracted from pud.h by tools/protoctl

This is a drift guard: the documentation is the specification, pud.h is the
implementation, and this test fails when the two disagree. It never invents a
value -- every expectation below is written in notes/usb-protocol.md.
"""

from common import PASS, FAIL, INCONCLUSIVE, ERROR, tool_json, result, decide

TEST = "test_protocol_spec"
KERNEL_REQUIRED = False
ORACLE = {
    "type": "SPEC",
    "source": "notes/usb-protocol.md",
    "expected": "documented protocol constants == pud.h",
}

# Every value below is stated in notes/usb-protocol.md.
EXPECTED_DEFINES = {
    "REQ_EP0_OUT": 0x00,
    "REQ_EP0_IN": 0x01,
    "REQ_EP1_OUT": 0x02,
    "REQ_EP2_IN": 0x03,
    "REQ_EP4_IN": 0x05,
    "USB_TRANS_MAX_SIZE": 65535,
    "PUD_EP1_HEADER_SIZE": 12,
    "PUD_DEFAULT_BAND_PIXELS": 21835,
    "PUD_CAPS_MAGIC": 0x43445550,
    "PUD_PROTO_VER": 2,
    "PUD_CAPS_V1_SIZE": 16,
    "PUD_CAPS_TOUCH": 0x0001,
    "PUD_TOUCH_VERSION": 1,
    "PUD_TOUCH_REPORT_SIZE": 8,
    "TYPE_VENDOR": 0x40,
    "PUD_DECODER_TJPGD": 0,
    "PUD_DECODER_JPEGDEC": 1,
    "PUD_DECODER_LZ4": 2,
    "PUD_DECODER_QOI": 3,
    "PUD_DECODER_RLE": 4,
    "PUD_DECODER_QOIZ": 5,
}

EXPECTED_STRUCT_SIZES = {
    "pud_ep1_header": 12,
    "pud_caps": 32,
}

EXPECTED_VID_PID = [0x2E8A, 0x0001]


def run():
    try:
        facts = tool_json("protoctl", "fields")
    except (EnvironmentError, ValueError) as exc:
        return result(TEST, INCONCLUSIVE, ORACLE, [], [str(exc)])

    checks = []
    for name, want in EXPECTED_DEFINES.items():
        got = facts.get("defines", {}).get(name)
        checks.append({"name": f"define {name}", "ok": got == want,
                       "observed": got, "expected": want})

    for name, want in EXPECTED_STRUCT_SIZES.items():
        got = facts.get("structs", {}).get(name, {}).get("size")
        checks.append({"name": f"sizeof {name}", "ok": got == want,
                       "observed": got, "expected": want})

    got_vid_pid = facts.get("vid_pid")
    checks.append({"name": "VID:PID", "ok": got_vid_pid == EXPECTED_VID_PID,
                   "observed": got_vid_pid, "expected": EXPECTED_VID_PID})

    return result(TEST, decide(checks), ORACLE, checks)
