# SPDX-License-Identifier: GPL-2.0-only
"""A loaded driver must present the connector and framebuffer the notes promise.

ORACLE: REQUIREMENT
SOURCE: notes/board-testing.md ("检查清单")
EXPECTED: at least one cardN-USB-* connector reports status=connected and
          enabled=enabled, and a framebuffer named "pud-drmdrmfb" exists.

KERNEL_REQUIRED: this only means anything with pud loaded (board, or
`make qemu`). With no pud nodes the result is INCONCLUSIVE, never FAIL: the
environment is absent, which is not a driver defect.
"""

from common import (PASS, FAIL, INCONCLUSIVE, ERROR, run_tool, tool_json,
                    result, decide, EXIT_ENV)

TEST = "test_fb_present"
KERNEL_REQUIRED = True
ORACLE = {
    "type": "REQUIREMENT",
    "source": "notes/board-testing.md 检查清单",
    "expected": "connector connected+enabled, fb named pud-drmdrmfb",
}


def run():
    rc, out, err = run_tool("pudctl", "nodes", "--json")
    if rc == EXIT_ENV:
        return result(TEST, INCONCLUSIVE, ORACLE, [],
                      ["no pud nodes: load the module (make qemu) first"])
    if rc != 0:
        return result(TEST, INCONCLUSIVE, ORACLE, [], [err.strip() or out.strip()])

    import json
    obs = json.loads(out)["observation"]
    connectors = obs.get("connectors", [])
    fbs = obs.get("framebuffers", [])

    checks = []
    any_connected = [c for c in connectors if c.get("status") == "connected"]
    checks.append({"name": "a connector is connected",
                   "ok": bool(any_connected),
                   "observed": [c.get("status") for c in connectors],
                   "expected": "includes 'connected'"})
    for c in any_connected:
        checks.append({"name": f"{c['path']} enabled",
                       "ok": c.get("enabled") == "enabled",
                       "observed": c.get("enabled"), "expected": "enabled"})
    checks.append({"name": "pud-drmdrmfb framebuffer exists",
                   "ok": bool(fbs), "observed": [f["path"] for f in fbs],
                   "expected": "at least one"})
    return result(TEST, decide(checks), ORACLE, checks)
