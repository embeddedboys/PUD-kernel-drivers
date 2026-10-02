// SPDX-License-Identifier: GPL-2.0-only
/*
 * fbctl -- actuate and inspect a Linux framebuffer.
 *
 * This is a TOOL, not a test: it performs the observation (draws a fill/rect/
 * string, or reports the framebuffer geometry) and never decides PASS/FAIL.
 * Tests interpret its output; see tests/README.md.
 *
 *   fbctl info  [--fd N] [--json]
 *   fbctl fill  [--fd N] [--color RRGGBB] [--border RRGGBB] [--buffer N|all]
 *               [--pan]
 *   fbctl rect  X Y W H [--fd N] [--color RRGGBB]
 *   fbctl text  X Y STR [--fd N] [--color RRGGBB]
 *
 * Common options: --help --version --json --quiet --verbose
 *
 * Exit codes (workspace convention):
 *   0 OK   1 FAIL   2 INVALID_USAGE   3 ENVIRONMENT_ERROR
 *
 * Derived from the two old test programs: the fill/border/buffer/pan portions
 * and the geometry dump come from tests/rect.c, the 8x16 text drawing from
 * tests/test_fb.c.  The pixel packer is new (the old one only handled 8-bit
 * channels) and all errors are now explicit.  tests/rect.c carried the
 * following notice, retained here as its licence requires for a derived work:
 *
 *   Copyright (c) 2014, Jumpnow Technologies, LLC
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *   Ideas taken from the Yocto Project psplash program.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "font8x16.h"

#define EXIT_FAIL 1
#define EXIT_USAGE 2
#define EXIT_ENV 3

static bool opt_json, opt_quiet, opt_verbose;
static int opt_fd = 0;

static void vprogressf(const char *fmt, va_list ap)
{
	if (!opt_quiet)
		vfprintf(stderr, fmt, ap);
}

static void progressf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprogressf(fmt, ap);
	va_end(ap);
}

static void die(int code, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(code);
}

struct fb {
	int fd;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	unsigned char *base;
	size_t maplen;
	char path[64];
};

static unsigned int pack_color(const struct fb *fb, unsigned int rgb)
{
	unsigned int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff,
		     b = rgb & 0xff;
	unsigned int v = 0;

	/* Scale each 8-bit channel to the channel's configured width. */
	if (fb->var.red.length)
		v |= ((r * ((1u << fb->var.red.length) - 1) + 127) / 255)
		     << fb->var.red.offset;
	if (fb->var.green.length)
		v |= ((g * ((1u << fb->var.green.length) - 1) + 127) / 255)
		     << fb->var.green.offset;
	if (fb->var.blue.length)
		v |= ((b * ((1u << fb->var.blue.length) - 1) + 127) / 255)
		     << fb->var.blue.offset;
	return v;
}

static void put_pixel(const struct fb *fb, int x, int y, unsigned int rgb)
{
	unsigned int val = pack_color(fb, rgb);
	size_t bytes = fb->var.bits_per_pixel / 8;
	unsigned char *p;

	if (x < 0 || y < 0 || (unsigned)x >= fb->var.xres ||
	    (unsigned)y >= fb->var.yres)
		return;
	p = fb->base + (size_t)y * fb->fix.line_length +
	    (size_t)x * bytes;
	switch (bytes) {
	case 1:
		*p = val;
		break;
	case 2:
		*(unsigned short *)p = val;
		break;
	case 4:
		*(unsigned int *)p = val;
		break;
	default:
		break;
	}
}

static void draw_rect(const struct fb *fb, int x, int y, int w, int h,
		      unsigned int rgb)
{
	int dx, dy;

	for (dy = 0; dy < h; dy++)
		for (dx = 0; dx < w; dx++)
			put_pixel(fb, x + dx, y + dy, rgb);
}

static void draw_char(const struct fb *fb, int x, int y, unsigned int rgb,
		      unsigned char c)
{
	const unsigned char *dots = &fontdata_8x16[(size_t)c * 16];
	int row, col;

	for (row = 0; row < 16; row++)
		for (col = 0; col < 8; col++)
			put_pixel(fb, x + col, y + row,
				  (dots[row] & (0x80 >> col)) ? rgb : 0);
}

/* Same wrapping rule as the old test_fb.c: 8 px per glyph; wrap before the
 * last 16 columns, and loop back to the top at y == 64. */
static void draw_string(const struct fb *fb, int x, int y, unsigned int rgb,
			const char *str)
{
	int cur_x = x, temp = 0;

	for (const unsigned char *s = (const unsigned char *)str; *s; s++) {
		if (!(*s & 0x80)) {
			draw_char(fb, cur_x + temp * 8, y, rgb, *s);
			temp++;
		}
		if ((int)fb->var.xres - (cur_x + temp * 8) <= 16) {
			cur_x = 0;
			y += 16;
			temp = 0;
		}
		if (y == 64) {
			cur_x = 0;
			y = 0;
		}
	}
}

static void fb_close(struct fb *fb)
{
	if (fb->base && fb->base != MAP_FAILED)
		munmap(fb->base, fb->maplen);
	if (fb->fd >= 0)
		close(fb->fd);
}

static void fb_open(struct fb *fb)
{
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	snprintf(fb->path, sizeof(fb->path), "/dev/fb%d", opt_fd);

	fb->fd = open(fb->path, O_RDWR);
	if (fb->fd < 0)
		die(EXIT_ENV, "fbctl: cannot open %s: %s", fb->path,
		    strerror(errno));
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0)
		die(EXIT_ENV, "fbctl: FBIOGET_VSCREENINFO: %s",
		    strerror(errno));
	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0)
		die(EXIT_ENV, "fbctl: FBIOGET_FSCREENINFO: %s",
		    strerror(errno));
	if (!fb->var.xres || !fb->var.yres || !fb->var.bits_per_pixel)
		die(EXIT_ENV, "fbctl: %s reports an empty mode", fb->path);

	fb->maplen = fb->fix.smem_len ? fb->fix.smem_len
				      : (size_t)fb->fix.line_length *
						fb->var.yres_virtual;
	fb->base = mmap(NULL, fb->maplen, PROT_READ | PROT_WRITE, MAP_SHARED,
			fb->fd, 0);
	if (fb->base == MAP_FAILED)
		die(EXIT_ENV, "fbctl: mmap(%zu): %s", fb->maplen,
		    strerror(errno));
}

static void print_info(const struct fb *fb)
{
	if (opt_json) {
		printf("{\"tool\":\"fbctl\",\"version\":\"1.0.0\",\"status\":\"ok\","
		       "\"device\":\"%s\",\"observation\":{"
		       "\"id\":\"%s\",\"xres\":%u,\"yres\":%u,\"yres_virtual\":%u,"
		       "\"bits_per_pixel\":%u,\"line_length\":%u,\"smem_len\":%u,"
		       "\"buffers\":%u,"
		       "\"red\":{\"offset\":%u,\"length\":%u},"
		       "\"green\":{\"offset\":%u,\"length\":%u},"
		       "\"blue\":{\"offset\":%u,\"length\":%u}}}\n",
		       fb->path, fb->fix.id, fb->var.xres, fb->var.yres,
		       fb->var.yres_virtual, fb->var.bits_per_pixel,
		       fb->fix.line_length, fb->fix.smem_len,
		       fb->var.yres ? fb->var.yres_virtual / fb->var.yres : 0,
		       fb->var.red.offset, fb->var.red.length,
		       fb->var.green.offset, fb->var.green.length,
		       fb->var.blue.offset, fb->var.blue.length);
		return;
	}
	printf("device        : %s (%s)\n", fb->path, fb->fix.id);
	printf("resolution    : %ux%u (virtual %u)\n", fb->var.xres,
	       fb->var.yres, fb->var.yres_virtual);
	printf("bits_per_pixel: %u\n", fb->var.bits_per_pixel);
	printf("line_length   : %u\n", fb->fix.line_length);
	printf("smem_len      : %u\n", fb->fix.smem_len);
	printf("buffers       : %u\n",
	       fb->var.yres ? fb->var.yres_virtual / fb->var.yres : 0);
	printf("rgb offsets   : r%d/%u g%d/%u b%d/%u\n", fb->var.red.offset,
	       fb->var.red.length, fb->var.green.offset,
	       fb->var.green.length, fb->var.blue.offset,
	       fb->var.blue.length);
}

static void usage(void)
{
	printf(
	    "fbctl -- actuate and inspect a Linux framebuffer\n"
	    "\n"
	    "usage: fbctl <command> [options]\n"
	    "\n"
	    "commands:\n"
	    "  info                       report framebuffer geometry\n"
	    "  fill                       fill the screen (or a border + inner rect)\n"
	    "  rect X Y W H               draw a filled rectangle\n"
	    "  text X Y STR               draw STR with the 8x16 font\n"
	    "\n"
	    "options:\n"
	    "  --fd N         framebuffer index (default 0)\n"
	    "  --color RRGGBB fill/rect/text colour (default ffffff)\n"
	    "  --border RRGGBB  fill: draw a 10 px border of this colour\n"
	    "  --buffer N|all fill: draw into buffer N, or all of them\n"
	    "  --pan          fill: FBIOPAN_DISPLAY for each buffer drawn\n"
	    "  --json         machine-readable output\n"
	    "  --quiet        suppress progress messages\n"
	    "  --verbose      more progress messages\n"
	    "  --version      print version\n"
	    "  --help         this text\n"
	    "\n"
	    "exit codes: 0 OK  1 FAIL  2 INVALID_USAGE  3 ENVIRONMENT_ERROR\n");
}

static unsigned int parse_hex(const char *s, const char *what)
{
	char *end;
	unsigned long v = strtoul(s, &end, 16);

	if (*s == '\0' || *end != '\0' || v > 0xffffff)
		die(EXIT_USAGE, "fbctl: --%s wants RRGGBB, got '%s'", what, s);
	return (unsigned int)v;
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "fd", required_argument, NULL, 1000 },
		{ "color", required_argument, NULL, 1001 },
		{ "border", required_argument, NULL, 1002 },
		{ "buffer", required_argument, NULL, 1003 },
		{ "pan", no_argument, NULL, 1004 },
		{ "json", no_argument, NULL, 1005 },
		{ "quiet", no_argument, NULL, 1006 },
		{ "verbose", no_argument, NULL, 1007 },
		{ "version", no_argument, NULL, 1008 },
		{ "help", no_argument, NULL, 1009 },
		{ NULL, 0, NULL, 0 },
	};
	const char *cmd;
	unsigned int color = 0xffffff, border = 0xffffffff;
	int buffer = 0, pan = 0, all = 0;
	struct fb fb;
	int opt, npos;
	char **pos;

	if (argc < 2) {
		usage();
		return EXIT_USAGE;
	}
	cmd = argv[1];
	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}
	if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
		printf("fbctl 1.0.0\n");
		return 0;
	}

	/* GNU getopt permutes argv[2..] so the non-option arguments (the command's
	 * own arguments) end up at the tail, starting at optind. */
	optind = 2;
	while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (opt) {
		case 1000:
			opt_fd = atoi(optarg);
			break;
		case 1001:
			color = parse_hex(optarg, "color");
			break;
		case 1002:
			border = parse_hex(optarg, "border");
			break;
		case 1003:
			if (!strcmp(optarg, "all"))
				all = 1;
			else
				buffer = atoi(optarg);
			break;
		case 1004:
			pan = 1;
			break;
		case 1005:
			opt_json = true;
			break;
		case 1006:
			opt_quiet = true;
			break;
		case 1007:
			opt_verbose = true;
			break;
		default:
			return EXIT_USAGE;
		}
	}
	pos = argv + optind;
	npos = argc - optind;

	fb_open(&fb);

	if (!strcmp(cmd, "info")) {
		print_info(&fb);
	} else if (!strcmp(cmd, "fill")) {
		int nbuf = fb.var.yres ? fb.var.yres_virtual / fb.var.yres : 1;
		int first = all ? 0 : buffer;
		int last = all ? nbuf - 1 : buffer;

		for (int idx = first; idx <= last; idx++) {
			size_t off = (size_t)fb.fix.line_length * fb.var.yres *
				     idx;
			unsigned char *shadow = fb.base;
			struct fb tmp = fb;

			tmp.base = shadow + off;
			if (border != 0xffffffff) {
				draw_rect(&tmp, 0, 0, fb.var.xres, fb.var.yres,
					  border);
				draw_rect(&tmp, 10, 10, fb.var.xres - 20,
					  fb.var.yres - 20, color);
			} else {
				draw_rect(&tmp, 0, 0, fb.var.xres, fb.var.yres,
					  color);
			}
			if (pan) {
				fb.var.yoffset = idx * fb.var.yres;
				if (ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.var) < 0)
					progressf("fbctl: FBIOPAN_DISPLAY: %s\n",
					     strerror(errno));
			}
		}
		progressf("fbctl: filled buffer%s %d..%d (%ux%u, color %06x)\n",
		     all ? "s" : "", first, last, fb.var.xres, fb.var.yres,
		     color);
	} else if (!strcmp(cmd, "rect")) {
		if (npos < 4)
			die(EXIT_USAGE,
			    "fbctl: rect needs X Y W H");
		draw_rect(&fb, atoi(pos[0]), atoi(pos[1]), atoi(pos[2]),
			  atoi(pos[3]), color);
		progressf("fbctl: rect %s %s %s %s color %06x\n", pos[0], pos[1],
		     pos[2], pos[3], color);
	} else if (!strcmp(cmd, "text")) {
		if (npos < 3)
			die(EXIT_USAGE, "fbctl: text needs X Y STR");
		draw_string(&fb, atoi(pos[0]), atoi(pos[1]), color, pos[2]);
		progressf("fbctl: text '%s' at %s,%s color %06x\n", pos[2], pos[0],
		     pos[1], color);
	} else {
		fb_close(&fb);
		die(EXIT_USAGE, "fbctl: unknown command '%s'", cmd);
	}

	if (opt_json && !strcmp(cmd, "fill"))
		printf("{\"tool\":\"fbctl\",\"version\":\"1.0.0\","
		       "\"status\":\"ok\",\"device\":\"%s\","
		       "\"observation\":{\"action\":\"fill\",\"buffers\":%d,"
		       "\"xres\":%u,\"yres\":%u,\"color\":\"%06x\"}}\n",
		       fb.path, all ? (int)(fb.var.yres_virtual / fb.var.yres)
				    : 1,
		       fb.var.xres, fb.var.yres, color);

	fb_close(&fb);
	return 0;
}
