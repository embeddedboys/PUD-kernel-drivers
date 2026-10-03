#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>

#include "encoder.h"
#include "jpegenc.h"
#include "pud.h"
#include "tinyc.h"

#define PUD_JPEG_MCU_SIZE 16
#define PUD_JPEG_MIN_CAPACITY 2048
#define PUD_JPEG_OUTPUT_RESERVE 512

/* Input rows are tightly packed; capacity describes output, not input.
 * The vendored 4:2:0 encoder reads complete 16x16 MCUs without edge padding,
 * so reject unaligned dimensions before it can read beyond the input.
 */
int jpeg_encode_rgb565(uint8_t *rgb565, u16 w, u16 h, size_t capacity,
                       uint8_t *work_buf, size_t *out_size, u8 quality)
{
	JPEGE_IMAGE *jpeg;
	JPEGENCODE jpe;
	int rc;

	if (!out_size)
		return -EINVAL;
	*out_size = 0;
	if (!rgb565 || !work_buf || !w || !h ||
	    capacity < PUD_JPEG_MIN_CAPACITY || capacity > INT_MAX || quality > JPEGE_Q_LOW ||
	    (w % PUD_JPEG_MCU_SIZE) || (h % PUD_JPEG_MCU_SIZE))
		return -EINVAL;
	jpeg = kzalloc(sizeof(*jpeg), GFP_KERNEL);
	if (!jpeg)
		return -ENOMEM;

	jpeg->pOutput = work_buf;
	jpeg->iBufferSize = capacity;
	jpeg->pHighWater = work_buf + capacity - PUD_JPEG_OUTPUT_RESERVE;
	rc = JPEGEncodeBegin(jpeg, &jpe, w, h, JPEGE_PIXEL_RGB565,
	                     JPEGE_SUBSAMPLE_420, quality);
	if (rc == JPEGE_SUCCESS)
		rc = JPEGAddFrame(jpeg, &jpe, rgb565, w * 2);
	if (rc == JPEGE_SUCCESS && JPEGEncodeEnd(jpeg) > 0) {
		*out_size = jpeg->iDataSize;
		rc = 0;
	} else {
		rc = rc == JPEGE_NO_BUFFER ? -ENOSPC : -EIO;
	}
	kfree(jpeg);
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

/* Each band is an independent raw-deflate stream. Level/window choices and
 * measured tradeoffs are documented in notes/encoders.md.
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

/*
 * DECODER_TYPE 6: QOI, then raw deflate against the previous band's QOI stream,
 * behind a 16-byte sub-header.
 *
 * The header is built in a local and copied in: `out` is
 * encoder_buf + PUD_EP1_HEADER_SIZE, which is four-byte aligned today, but
 * nothing in the plumbing promises that.
 */
int qoid_pack(const u8 *qoi, size_t qoi_len, const u8 *dict, size_t dict_len,
              u32 dict_serial, bool keyframe, u8 *out, size_t out_cap,
              size_t *out_size, void *work)
{
	struct pud_qoid_header h;
	size_t n;

	if (!qoi || !qoi_len || !out || !out_size || !work)
		return -EINVAL;
	if (dict_len && !dict)
		return -EINVAL;
	if (dict_len + qoi_len > TINYC_WINDOW)
		/* The device's window would not hold it either; the band is
		 * refused there rather than truncated here. */
		return -ENOSPC;
	if (qoi_len > PUD_QOID_DICT_MAX)
		/*
		 * The window is history | output, half each, so the band's own QOI
		 * stream has to fit the output half.  Say so here: the device would
		 * drop the band as oversize and count g_decoder_stat_qoid_oversize,
		 * which the driver never sees, so that part of the panel would
		 * silently stop updating.  pud_apply_caps() sizes the bands to
		 * prevent this, so reaching here means that budget is wrong.
		 */
		return -E2BIG;
	if (out_cap <= PUD_QOID_HDR_SIZE)
		return -ENOSPC;

	n = tinyc_deflate(dict, dict_len, qoi, qoi_len, out + PUD_QOID_HDR_SIZE,
	                  out_cap - PUD_QOID_HDR_SIZE, work);
	if (!n)
		return -ENOSPC;

	h.magic = PUD_QOID_MAGIC;
	h.flags = keyframe ? PUD_QOID_F_KEYFRAME : PUD_QOID_F_DELTA;
	h.reserved = 0;
	h.dict_serial = keyframe ? 0 : dict_serial;
	h.dict_len = (u32)dict_len;
	memcpy(out, &h, sizeof(h));

	*out_size = PUD_QOID_HDR_SIZE + n;
	return 0;
}
