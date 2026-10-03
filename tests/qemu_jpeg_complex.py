# SPDX-License-Identifier: GPL-2.0-only
"""Compatibility entry point for the complex QEMU JPEG test."""

import sys

from qemu_jpeg_check import main


if __name__ == "__main__":
    # Preserve the previously documented positional fixture directory.
    sys.exit(main(["--images", *sys.argv[1:]]))
