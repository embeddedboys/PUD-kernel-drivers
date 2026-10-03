// SPDX-License-Identifier: GPL-2.0-only
/* Allocation/type shims only: jpeg_wrapper.c is extracted from encoder.c. */
#include "jpegenc.h"
#include <errno.h>
#include <stdio.h>

typedef uint16_t u16;
typedef uint8_t u8;
#define GFP_KERNEL 0
#define kzalloc(n, flags) calloc(1, n)
#define kfree free
#include "jpeg_wrapper.c"

#define WIDTH 800
#define HEIGHT 480
#define OUTPUT_CAPACITY 65522

enum pattern {
	PATTERN_RED,
	PATTERN_GRADIENT,
	PATTERN_NOISE,
};

static void fill_pixels(uint16_t *pixels, enum pattern pattern)
{
	unsigned int seed = 17;
	size_t i;

	for (i = 0; i < WIDTH * HEIGHT; i++) {
		seed = seed * 1664525 + 1013904223;
		switch (pattern) {
		case PATTERN_RED:
			pixels[i] = 0xf800;
			break;
		case PATTERN_GRADIENT:
			pixels[i] = (i % WIDTH) / 25 * 0x801;
			break;
		case PATTERN_NOISE:
			pixels[i] = seed >> 16;
			break;
		}
	}
}

static int save_jpeg(const char *directory, enum pattern pattern,
                     const uint8_t *output, size_t size)
{
	char path[1024];
	FILE *file;
	int rc;

	snprintf(path, sizeof(path), "%s/%d.jpg", directory, pattern);
	file = fopen(path, "wb");
	if (!file)
		return 1;
	rc = fwrite(output, 1, size, file) != size;
	if (fclose(file))
		rc = 1;
	return rc;
}

static int check_pattern(uint16_t *pixels, uint8_t *output,
                         enum pattern pattern, const char *directory)
{
	size_t size = 0;
	int rc;

	fill_pixels(pixels, pattern);
	rc = jpeg_encode_rgb565((uint8_t *)pixels, WIDTH, HEIGHT,
	                       OUTPUT_CAPACITY, output, &size, JPEGE_Q_LOW);
	if (pattern == PATTERN_NOISE)
		return rc != -ENOSPC || size != 0;
	if (rc || size < 2 || output[size - 2] != 0xff || output[size - 1] != 0xd9)
		return 1;
	return save_jpeg(directory, pattern, output, size);
}

static int check_invalid_inputs(uint16_t *pixels, uint8_t *output)
{
	size_t size = 123;
	int rc;

	rc = jpeg_encode_rgb565((uint8_t *)pixels, WIDTH - 1, HEIGHT,
	                       OUTPUT_CAPACITY, output, &size, JPEGE_Q_LOW);
	if (rc != -EINVAL || size)
		return 1;
	size = 123;
	rc = jpeg_encode_rgb565((uint8_t *)pixels, WIDTH, HEIGHT, 1024,
	                       output, &size, JPEGE_Q_LOW);
	return rc != -EINVAL || size != 0;
}

int main(int argc, char **argv)
{
	uint16_t *pixels;
	uint8_t *output;
	enum pattern pattern;
	int rc = 1;

	if (argc != 2)
		return 2;
	pixels = calloc(WIDTH * HEIGHT, sizeof(*pixels));
	output = calloc(OUTPUT_CAPACITY, 1);
	if (!pixels || !output)
		goto free_buffers;
	for (pattern = PATTERN_RED; pattern <= PATTERN_NOISE; pattern++) {
		if (check_pattern(pixels, output, pattern, argv[1])) {
			fprintf(stderr, "JPEG pattern %d failed\n", pattern);
			goto free_buffers;
		}
	}
	rc = check_invalid_inputs(pixels, output);
free_buffers:
	free(pixels);
	free(output);
	return rc;
}
