# SPDX-License-Identifier: GPL-2.0-only
"""The built module must carry the metadata the loader and notes require.

ORACLE: SPEC
SOURCE: notes/build-and-test.md (vermagic), notes/pitfalls.md 4.4 (depends)
EXPECTED:
    vermagic present and non-empty
    depends lists drm_dma_helper on the 7.0 branch (that is what makes
    `insmod pud.ko` fail with "Unknown symbol in module" when it is skipped)

The check is deliberately target-agnostic: pud.ko may have been built for a
different kernel than the host is running (the repo builds for three kernels),
so a mismatch with `uname -r` is reported as an observation, not a failure.
Use `tools/pudctl vermagic` when you specifically want the loadability check.
No module present -> INCONCLUSIVE (nothing to inspect).
"""

import os

from common import PASS, FAIL, INCONCLUSIVE, ERROR, tool_json, result, decide, ROOT

TEST = "test_modinfo"
KERNEL_REQUIRED = False
ORACLE = {
    "type": "SPEC",
    "source": "notes/build-and-test.md; notes/pitfalls.md 4.4",
    "expected": "vermagic present; depends includes drm_dma_helper on 7.0",
}


def run():
    ko = os.path.join(ROOT, "pud.ko")
    if not os.path.exists(ko):
        return result(TEST, INCONCLUSIVE, ORACLE, [],
                      [f"no {ko}; run `make modules` before this check"])

    try:
        info = tool_json("pudctl", "modinfo", "--ko", ko)["observation"]
    except (EnvironmentError, ValueError) as exc:
        return result(TEST, INCONCLUSIVE, ORACLE, [], [str(exc)])

    checks = []
    vermagic = info.get("vermagic", "")
    checks.append({"name": "vermagic present", "ok": bool(vermagic),
                   "observed": vermagic, "expected": "non-empty"})

    vm_release = vermagic.split()[0] if vermagic else ""
    running = os.uname().release
    depends = [d for d in info.get("depends", "").replace(" ", "").split(",")
               if d]
    if vm_release.startswith("7.0"):
        checks.append({"name": "depends includes drm_dma_helper",
                       "ok": "drm_dma_helper" in depends,
                       "observed": depends,
                       "expected": ["drm_dma_helper"]})

    notes = [f"vermagic target {vm_release or '?'}, running kernel {running} "
             f"({'match' if vm_release == running else 'different target'})"]
    return result(TEST, decide(checks), ORACLE, checks, notes)
