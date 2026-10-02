#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the pud test suite and aggregate the results.

    tests/run_tests.py [--list] [--json] [--include-kernel] [--test NAME]

Offline tests run by default. Tests marked KERNEL_REQUIRED (they need pud
loaded, i.e. a board or `make qemu`) are skipped unless --include-kernel is
given; skipped is not the same as passed.

Aggregate exit code (workspace convention):
    0 all PASS     1 any FAIL     3 any ERROR     5 any INCONCLUSIVE
"""

import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

EXIT_OK, EXIT_FAIL, EXIT_ENV, EXIT_INCONCLUSIVE = 0, 1, 3, 5


def discover():
    names = sorted(f[:-3] for f in os.listdir(HERE)
                   if f.startswith("test_") and f.endswith(".py"))
    modules = []
    for name in names:
        path = os.path.join(HERE, name + ".py")
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        modules.append(module)
    return modules


def aggregate(results, skipped):
    statuses = [r["status"] for r in results]
    if "FAIL" in statuses:
        return EXIT_FAIL
    if "ERROR" in statuses:
        return EXIT_ENV
    if "INCONCLUSIVE" in statuses:
        return EXIT_INCONCLUSIVE
    return EXIT_OK


def print_text(results, skipped, json_mode):
    if json_mode:
        print(json.dumps({"results": results, "skipped": skipped}, indent=2))
        return
    for r in results:
        print(f"[TEST] {r['test']}")
        print(f"[ORACLE] {r['oracle']['type']}")
        print(f"[SOURCE] {r['oracle']['source']}")
        for c in r["checks"]:
            mark = "ok" if c["ok"] else "XX"
            print(f"  [{mark}] {c['name']}: observed={c['observed']!r} "
                  f"expected={c['expected']!r}")
        for note in r.get("notes", []):
            print(f"  note: {note}")
        print(f"[RESULT] {r['status']}\n")
    for name in skipped:
        print(f"[SKIP] {name} (kernel-required; use --include-kernel)")
    passed = sum(1 for r in results if r["status"] == "PASS")
    print(f"== {passed}/{len(results)} PASS, {len(skipped)} skipped ==")


def main(argv):
    parser = argparse.ArgumentParser(prog="run_tests",
                                     description="pud test suite runner")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--include-kernel", action="store_true",
                        help="also run tests that need pud loaded")
    parser.add_argument("--test", action="append", default=None,
                        help="run only this test (repeatable)")
    args = parser.parse_args(argv)

    modules = discover()
    if args.list:
        for m in modules:
            kind = "kernel" if m.KERNEL_REQUIRED else "offline"
            print(f"{m.TEST:28} {kind:8} {m.ORACLE['type']:12} "
                  f"{m.ORACLE['source']}")
        return EXIT_OK

    selected = [m for m in modules
                if not args.test or m.TEST in args.test]
    results, skipped = [], []
    for m in selected:
        if m.KERNEL_REQUIRED and not args.include_kernel:
            skipped.append(m.TEST)
            continue
        try:
            results.append(m.run())
        except Exception as exc:  # noqa: BLE001 - report, never crash the suite
            results.append({"test": m.TEST, "status": "ERROR",
                            "oracle": m.ORACLE, "checks": [],
                            "notes": [f"unhandled exception: {exc!r}"]})

    print_text(results, skipped, args.json)
    return aggregate(results, skipped)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
