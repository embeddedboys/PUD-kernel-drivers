#ifndef __ENCODER_H
#define __ENCODER_H

#include <linux/kernel.h>

#include "jpegenc.h"
#include "rgb565_qoi.h"
#include "rgb565_rle.h"

int jpeg_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t capacity,
                       uint8_t *work_buf, size_t *out_size, u8 quality);
int qoi_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size,
                      uint8_t *work_buf, size_t *out_size);
int rle_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size,
                      uint8_t *work_buf, size_t *out_size);

struct z_stream_s;

struct z_stream_s *qoiz_stream_alloc(void);
void qoiz_stream_free(struct z_stream_s *strm);
int qoiz_deflate(struct z_stream_s *strm, const uint8_t *qoi, size_t qoi_size,
                 uint8_t *out, size_t out_cap, size_t *out_size);

/*
 * DECODER_TYPE 6 (PUD_DECODER_QOID): the band's QOI stream, deflated with
 * `dict` (dict_len bytes of history) as its preset dictionary, behind the
 * 16-byte sub-header of struct pud_qoid_header.  Unusable with the kernel's
 * zlib -- no Z_FIXED, no deflateSetDictionary -- so tinyc.c does the deflate.
 *
 * `keyframe` writes QOID_F_KEYFRAME and a zero dict_serial instead of
 * QOID_F_DELTA, which is what a band with no dictionary candidate sends.
 * Returns 0 and sets *out_size, -EINVAL, or -ENOSPC when the result does not
 * fit out_cap (the caller turns that into needs_full_refresh).
 */
int qoid_pack(const u8 *qoi, size_t qoi_len, const u8 *dict, size_t dict_len,
              u32 dict_serial, bool keyframe, u8 *out, size_t out_cap,
              size_t *out_size, void *work);

#endif