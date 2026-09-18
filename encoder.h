#ifndef __ENCODER_H
#define __ENCODER_H

#include <linux/kernel.h>

#include "jpegenc.h"
#include "rgb565_qoi.h"
#include "rgb565_rle.h"

int jpeg_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t len,
		       uint8_t *work_buf, size_t *out_size, u8 quality);
int qoi_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size,
		      uint8_t *work_buf, size_t *out_size);
int rle_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size,
		      uint8_t *work_buf, size_t *out_size);

#endif