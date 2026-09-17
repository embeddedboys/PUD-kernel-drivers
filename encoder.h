#ifndef __ENCODER_H
#define __ENCODER_H

#include <linux/kernel.h>

#include "jpegenc.h"
#include "rgb565_qoi.h"

uint8_t *jpeg_encode_bmp(uint8_t *bmp, size_t len, size_t *out_size);
int jpeg_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t len, uint8_t *work_buf, size_t *out_size, u8 quality);
int qoi_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size, uint8_t *work_buf, size_t *out_size);

#endif