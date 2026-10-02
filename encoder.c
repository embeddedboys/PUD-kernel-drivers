#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>

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

/*
 * QOI + raw deflate (PUD_DECODER_QOIZ).  The deflate stage is the kernel's own
 * lib/zlib_deflate, so no encoder is vendored for it; the caller QOI-encodes the
 * band first and hands the QOI stream in here.
 *
 * Parameters, measured on the desktop regions of the firmware repo's
 * desktop_codecs.py (sizes against QOI alone, desktop / photo wallpaper):
 *
 *   level 1, 32 KB window, memLevel 8   -29.6% / -25.0%   workspace ~256 KB
 *   level 1,  4 KB window, memLevel 6   -29.0% / -24.8%   workspace  ~48 KB
 *
 * A band is at most one 64 KB transfer, so the big window buys nothing; 4 KB
 * and memLevel 6 keep nearly all of it for a fifth of the memory.  Level 1
 * because it already has nearly all of the gain (level 6 adds ~1.5%).  Negative
 * windowBits is zlib's way of asking for a raw stream: no zlib header, no
 * adler32 -- the device's inflate expects exactly that.
 */
#define QOIZ_LEVEL 1
#define QOIZ_WBITS 12
#define QOIZ_MEMLEVEL 6

struct z_stream_s *qoiz_stream_alloc(void)
{
	struct z_stream_s *strm;

	strm = kzalloc(sizeof(*strm), GFP_KERNEL);
	if (!strm)
		return NULL;

	strm->workspace =
	        vzalloc(zlib_deflate_workspacesize(-QOIZ_WBITS, QOIZ_MEMLEVEL));
	if (!strm->workspace) {
		kfree(strm);
		return NULL;
	}

	if (zlib_deflateInit2(strm, QOIZ_LEVEL, Z_DEFLATED, -QOIZ_WBITS,
	                      QOIZ_MEMLEVEL, Z_DEFAULT_STRATEGY) != Z_OK) {
		vfree(strm->workspace);
		kfree(strm);
		return NULL;
	}

	return strm;
}

void qoiz_stream_free(struct z_stream_s *strm)
{
	if (!strm)
		return;

	zlib_deflateEnd(strm);
	vfree(strm->workspace);
	kfree(strm);
}

/* Deflate one QOI stream (one band) in a single call.  Every band is a stream
 * of its own -- the device inflates each transfer from a clean state -- so the
 * stream is reset first rather than carried over from the previous band. */
int qoiz_deflate(struct z_stream_s *strm, const uint8_t *qoi, size_t qoi_size,
                 uint8_t *out, size_t out_cap, size_t *out_size)
{
	int rc;

	if (!strm || !qoi || !qoi_size || !out || !out_size)
		return -EINVAL;

	if (zlib_deflateReset(strm) != Z_OK)
		return -EIO;

	strm->next_in = qoi;
	strm->avail_in = qoi_size;
	strm->next_out = out;
	strm->avail_out = out_cap;

	rc = zlib_deflate(strm, Z_FINISH);
	if (rc != Z_STREAM_END)
		/* Z_OK here means the output did not fit: deflate can expand
		 * incompressible input by a few bytes per 16 KB block */
		return rc == Z_OK ? -ENOSPC : -EIO;

	*out_size = strm->total_out;
	return 0;
}
