// SPDX-License-Identifier: GPL-2.0-only
/* ORACLE: INVARIANT -- notes/usb-protocol.md payload bounds and framing. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define ALIGN(size, alignment) (((size) + (alignment) - 1) & ~((alignment) - 1))
#include "frame_header.h"

struct pud {
	u8 *encoder_buf;
	size_t encoder_buf_size;
	u32 frame_max;
};
#include "frame_prepare.c"

int main(void)
{
	u8 *source = malloc(3);
	u8 *output = malloc(64);
	struct pud pud = { output, 64, 16 };
	struct pud_ep1_header *header = (void *)output;
	int rc;

	assert(source && output);
	memcpy(source, "abc", 3);
	memset(output, 0xa5, 64);
	rc = pud_prepare_frame(&pud, 1, 2, 3, 4, source, 3);
	assert(rc == 16);
	assert(header->xs == 1 && header->ys == 2);
	assert(header->xe == 3 && header->ye == 4 && header->size == 4);
	assert(!memcmp(output + PUD_EP1_HEADER_SIZE, source, 3));
	assert(output[15] == 0 && output[16] == 0xa5);

	memset(output, 0xa5, 64);
	assert(pud_prepare_frame(&pud, 0, 0, 1, 1, source, 5) == -EMSGSIZE);
	assert(output[0] == 0xa5); /* Refusal must not change the outgoing buffer. */
	assert(pud_prepare_frame(&pud, 0, 0, 1, 1, source, SIZE_MAX) == -EMSGSIZE);
	assert(pud_prepare_frame(&pud, 0, 0, 1, 1, NULL, 1) == -EMSGSIZE);

	pud.frame_max = 64;
	pud.encoder_buf_size = 15;
	assert(pud_payload_capacity(&pud) == 2);
	assert(pud_prepare_frame(&pud, 0, 0, 1, 1, source, 3) == -EMSGSIZE);
	pud.encoder_buf_size = PUD_EP1_HEADER_SIZE;
	assert(pud_payload_capacity(&pud) == 0);
	assert(pud_prepare_frame(&pud, 0, 0, 1, 1, source, 1) == -EMSGSIZE);

	pud.encoder_buf_size = 64;
	memcpy(output + PUD_EP1_HEADER_SIZE, "abc", 3);
	assert(pud_prepare_frame(&pud, 0, 0, 1, 1,
	                         output + PUD_EP1_HEADER_SIZE, 3) == 16);
	assert(!memcmp(output + PUD_EP1_HEADER_SIZE, "abc\0", 4));
	free(source);
	free(output);
	return 0;
}
