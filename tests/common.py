# SPDX-License-Identifier: GPL-2.0-only
"""Shared helpers for the tests. Deliberately tiny -- no framework.

Tests call the reusable tools in tools/ and interpret their facts against an
explicit oracle. A test module exposes:

    TEST            stable name (test_<behaviour>)
    KERNEL_REQUIRED bool -- needs a loaded module / board or guest
    ORACLE          {"type", "source", "expected"} per ../AGENTS.md
    run()           -> result dict

Result dict:
    {"test", "status", "oracle", "checks": [{"name", "ok", "observed",
     "expected"}], "notes": [...]}

Status: PASS / FAIL / INCONCLUSIVE / ERROR.
"""

import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")

PASS, FAIL, INCONCLUSIVE, ERROR = "PASS", "FAIL", "INCONCLUSIVE", "ERROR"

# Tool exit codes (workspace convention).
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_ENV, EXIT_TIMEOUT, EXIT_INCONCLUSIVE = \
    0, 1, 2, 3, 4, 5


def run_tool(name, *args, timeout=30):
    """Run tools/<name> (a python script) and return (rc, stdout, stderr)."""
    path = os.path.join(TOOLS, name)
    if not os.path.exists(path):
        raise EnvironmentError(f"tool not found: {path}")
    try:
        proc = subprocess.run([sys.executable, path, *args],
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return EXIT_TIMEOUT, "", f"{name} timed out after {timeout}s"
    return proc.returncode, proc.stdout, proc.stderr


def tool_json(name, *args, timeout=30):
    """Run a tool with --json and parse its stdout. Raises on tool errors."""
    rc, out, err = run_tool(name, *args, "--json", timeout=timeout)
    if rc != EXIT_OK:
        raise EnvironmentError(f"{name} exited {rc}: {err.strip() or out.strip()}")
    return json.loads(out)


def result(test, status, oracle, checks, notes=None):
    return {"test": test, "status": status, "oracle": oracle,
            "checks": checks, "notes": notes or []}


def decide(checks):
    """PASS when every check holds, FAIL otherwise."""
    return PASS if all(c["ok"] for c in checks) else FAIL
