/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tinyc -- a raw-deflate encoder that emits only fixed-Huffman and stored
 * blocks, and takes the bytes before the input as history.
 *
 * The counterpart of the firmware's tinyd (Pico-USB-Display/src/decoders/
 * tinyd/): that decoder accepts stored and fixed-Huffman blocks only -- a
 * dynamic Huffman block is reported as an error, not decoded -- and it decodes
 * into a caller-supplied window whose first dict_len bytes are already known.
 * Neither is available from the kernel's own zlib: include/linux/zlib.h has no
 * Z_FIXED and no deflateSetDictionary, and the vendored miniz in the firmware
 * repository is inflate only (miniz_tinfl), so the host side of DECODER_TYPE 6
 * needs an encoder of its own.
 *
 * No zlib header and no adler32: a raw stream, the same thing -windowBits asks
 * of zlib.  Verified against tinyd on the host: every case in
 * tools/tinyc-test round-trips, and for inputs where no better match exists
 * the bytes are identical to zlib's Z_FIXED output.
 */
#ifndef __TINYC_H
#define __TINYC_H

#include <linux/kernel.h>
#include <linux/types.h>

/*
 * Encode `in` as a raw-deflate stream into `out`, with `dict` (dict_len bytes)
 * as history: matches may reach back into it, and a decoder given the same
 * bytes as its preset window reconstructs the stream.  Returns the number of
 * bytes written, or 0 if it does not fit in out_max, or on bad arguments.
 *
 * `work` is scratch of tinyc_work_size() bytes; it is large (128 KB), so
 * allocate it once per encoder with vzalloc() rather than per band.
 *
 * dict_len + in_len must not exceed TINYC_WINDOW: a DEFLATE distance cannot
 * reach further back than 32768, and the protocol's own PUD_DELTA_WIN is that
 * number with a band at most half of it.
 */
#define TINYC_WINDOW 32768u

size_t tinyc_deflate(const u8 *dict, size_t dict_len, const u8 *in,
		     size_t in_len, u8 *out, size_t out_max, void *work);

size_t tinyc_work_size(void);

/* The largest output tinyc_deflate() can produce for in_len input bytes:
 * fixed-Huffman coding can expand, and the stored-block fallback adds at most
 * five bytes per 65535. */
size_t tinyc_bound(size_t in_len);

#endif /* __TINYC_H */
