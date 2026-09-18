#include <linux/slab.h>
#include <linux/mm.h>

#include "encoder.h"
#include "jpegenc.h"

int jpeg_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t len,
		       uint8_t *work_buf, size_t *out_size, u8 quality)
{
	int rc, bits;
	int pitch, bytewidth;
	size_t buffer_size;
	JPEGE_IMAGE jpeg;
	JPEGENCODE jpe;

	bits = 16;

	bytewidth = (w * bits) >> 3;
	pitch = (bytewidth + 3) & 0xfffc;
	// printk("%s, w : %d, h : %d, pitch : %d\n", __func__, w, h, pitch);

	buffer_size = len;

	memset(&jpeg, 0, sizeof(JPEGE_IMAGE));
	jpeg.pOutput = work_buf;
	jpeg.iBufferSize = buffer_size;
	jpeg.pHighWater = &jpeg.pOutput[jpeg.iBufferSize - 512];

	rc = JPEGEncodeBegin(&jpeg, &jpe, w, h, JPEGE_PIXEL_RGB565,
			     JPEGE_SUBSAMPLE_420, quality);
	if (rc == JPEGE_SUCCESS)
		JPEGAddFrame(&jpeg, &jpe, rgb565, pitch);

	JPEGEncodeEnd(&jpeg);
	// printk("%s, jpeg size : %d\n", __func__, jpeg.iDataSize);
	*out_size = jpeg.iDataSize;

	return rc;
}

int qoi_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size,
		      uint8_t *work_buf, size_t *out_size)
{
	size_t sz;

	if (!rgb565 || !work_buf || !w || !h || !out_size)
		return -EINVAL;

	sz = rgb565_qoi_compress((const uint16_t *)rgb565, (size_t)w * h,
				 work_buf, work_size);
	if (sz == 0)
		return -ENOSPC;

	*out_size = sz;
	return 0;
}

/* Same worst case as QOI (3 bytes per pixel plus framing), so the band budget
 * the device reports covers both. */
int rle_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t work_size,
		      uint8_t *work_buf, size_t *out_size)
{
	size_t sz;

	if (!rgb565 || !work_buf || !w || !h || !out_size)
		return -EINVAL;

	sz = rgb565_rle_compress((const uint16_t *)rgb565, (size_t)w * h,
				 work_buf, work_size);
	if (sz == 0)
		return -ENOSPC;

	*out_size = sz;
	return 0;
}
