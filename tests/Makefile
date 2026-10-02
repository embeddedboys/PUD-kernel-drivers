# SPDX-License-Identifier: GPL-2.0-only
# The framebuffer programs moved to ../tools/fbctl.c; what is left here is the
# test suite that interprets tool output.
PYTHON ?= python3

all: test

test:
	$(PYTHON) run_tests.py

kernel-test:
	$(PYTHON) run_tests.py --include-kernel

clean:
	rm -rf __pycache__

.PHONY: all test kernel-test clean
