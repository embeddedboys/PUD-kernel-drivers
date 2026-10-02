# SPDX-License-Identifier: GPL-2.0-only
# Userspace tools.  fbctl talks to /dev/fbN (board or guest); the *ctl scripts
# are plain host-side (pudctl/protoctl) or root-only capture (usbmonctl).
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: fbctl

fbctl: fbctl.c font8x16.h
	$(CC) $(CFLAGS) -o $@ fbctl.c

clean:
	rm -f fbctl

.PHONY: all clean
