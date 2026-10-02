// SPDX-License-Identifier: GPL-2.0-only
/* tinyc -- see tinyc.h.  The encoder counterpart of the firmware's tinyd. */
#include "tinyc.h"

#include <linux/string.h>

/* Window: dict_len + in_len must fit, because a DEFLATE distance cannot exceed
 * 32768 and this encoder indexes its match chains by position.  The protocol's
 * own PUD_DELTA_WIN is 32768 and a band is at most half of it, so the caller
 * cannot exceed this without changing the protocol. */
#define TINYC_WINDOW 32768u
#define TINYC_WMASK  (TINYC_WINDOW - 1u)
#define TINYC_HASH_BITS 15
#define TINYC_HASH_SIZE (1u << TINYC_HASH_BITS)
#define TINYC_MAX_CHAIN 32
#define TINYC_MIN_MATCH 3
#define TINYC_MAX_MATCH 258

/* RFC 1951 3.2.5, the same tables tinyd decodes with. */
static const u16 len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
	59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
	4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const u16 dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
	513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const u8 dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
	9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

struct tinyc_work {
	/* Chain links, position + 1 so that 0 means "none".  Positions fit in 16
	 * bits because the window does; that halves the scratch. */
	u16 head[TINYC_HASH_SIZE];
	u16 prev[TINYC_WINDOW];
};

size_t tinyc_work_size(void)
{
	return sizeof(struct tinyc_work);
}

size_t tinyc_bound(size_t in_len)
{
	/* Fixed-Huffman worst case is a little over 9 bits per literal, and the
	 * stored fallback needs five bytes per block.  Take the larger of the two
	 * plus slack, so a caller can size an output buffer once. */
	size_t fixed = in_len + in_len / 8 + 64;
	size_t stored = in_len + 5 * (in_len / 65535u + 1) + 8;

	return fixed > stored ? fixed : stored;
}

/* ------------------------------------------------------------------ bits -- */

struct bw {
	u8 *out;
	size_t cap;
	size_t n;
	u32 acc;
	unsigned nbits;
	int over;
};

static void bw_bits(struct bw *b, u32 v, unsigned n)
{
	if (b->over)
		return;
	b->acc |= v << b->nbits;
	b->nbits += n;
	while (b->nbits >= 8) {
		if (b->n >= b->cap) {
			b->over = 1;
			return;
		}
		b->out[b->n++] = (u8)(b->acc & 0xffu);
		b->acc >>= 8;
		b->nbits -= 8;
	}
}

/* Huffman codes go out most-significant bit first; raw values (length and
 * distance extra bits) go out least-significant first, which is what bw_bits
 * does. */
static void bw_code(struct bw *b, unsigned code, unsigned n)
{
	unsigned r = 0, i;

	for (i = 0; i < n; i++)
		r = (r << 1) | ((code >> i) & 1u);
	bw_bits(b, r, n);
}

static void bw_align(struct bw *b)
{
	if (b->over)
		return;
	if (b->nbits) {
		if (b->n >= b->cap) {
			b->over = 1;
			return;
		}
		b->out[b->n++] = (u8)(b->acc & 0xffu);
		b->acc = 0;
		b->nbits = 0;
	}
}

/* The fixed literal/length code of RFC 1951 3.2.6. */
static void bw_fixed_lit(struct bw *b, unsigned sym)
{
	if (sym < 144)
		bw_code(b, 0x30u + sym, 8);
	else if (sym < 256)
		bw_code(b, 0x190u + (sym - 144), 9);
	else if (sym < 280)
		bw_code(b, sym - 256, 7);
	else
		bw_code(b, 0xC0u + (sym - 280), 8);
}

static int len_index(unsigned len)
{
	int i;

	for (i = 28; i >= 0; i--)
		if (len >= len_base[i])
			return i;
	return 0;
}

static int dist_index(unsigned d)
{
	int i;

	for (i = 29; i >= 0; i--)
		if (d >= dist_base[i])
			return i;
	return 0;
}

/* ---------------------------------------------------------------- input -- */

/* The window is dict then in, without copying either. */
static u8 win_byte(const u8 *dict, size_t dict_len, const u8 *in,
			size_t p)
{
	return p < dict_len ? dict[p] : in[p - dict_len];
}

static unsigned hash3(const u8 *dict, size_t dict_len, const u8 *in,
		      size_t p)
{
	unsigned h = (unsigned)win_byte(dict, dict_len, in, p) << 16 |
		     (unsigned)win_byte(dict, dict_len, in, p + 1) << 8 |
		     (unsigned)win_byte(dict, dict_len, in, p + 2);

	return (h * 2654435761u) >> (32 - TINYC_HASH_BITS);
}

static void chain_insert(struct tinyc_work *w, const u8 *dict,
			 size_t dict_len, const u8 *in, size_t p)
{
	unsigned h = hash3(dict, dict_len, in, p);

	w->prev[p & TINYC_WMASK] = w->head[h];
	w->head[h] = (u16)(p + 1);
}

/* ---------------------------------------------------------------- blocks -- */

static size_t emit_fixed(const u8 *dict, size_t dict_len, const u8 *in,
			 size_t in_len, u8 *out, size_t out_max,
			 struct tinyc_work *w)
{
	struct bw b = { out, out_max, 0, 0, 0, 0 };
	size_t total = dict_len + in_len;
	size_t p, seed, i;

	memset(w->head, 0, sizeof w->head);
	memset(w->prev, 0, sizeof w->prev);

	/* The dictionary has to be in the chains before the first input byte, or
	 * nothing can reference it -- that is the whole point of the codec. */
	seed = dict_len > 2 ? dict_len - 2 : 0;
	for (i = 0; i < seed; i++)
		chain_insert(w, dict, dict_len, in, i);

	/* BFINAL = 1, BTYPE = 01 (fixed Huffman), then everything in one block. */
	bw_bits(&b, 1, 1);
	bw_bits(&b, 1, 2);

	p = dict_len;
	while (p < total && !b.over) {
		unsigned best_len = 0, best_dist = 0;
		unsigned h = (p + 2 < total) ? hash3(dict, dict_len, in, p) : 0;
		u32 cand = (p + 2 < total) ? w->head[h] : 0;
		int chain = TINYC_MAX_CHAIN;

		while (cand && chain--) {
			size_t cp = (size_t)cand - 1;
			size_t dist = p - cp;
			size_t max = total - p;
			size_t l = 0;

			if (dist == 0 || dist > TINYC_WINDOW)
				break;
			if (max > TINYC_MAX_MATCH)
				max = TINYC_MAX_MATCH;
			while (l < max && win_byte(dict, dict_len, in, cp + l) ==
			                          win_byte(dict, dict_len, in, p + l))
				l++;
			if (l > best_len) {
				best_len = (unsigned)l;
				best_dist = (unsigned)dist;
				if (l >= max)
					break;
			}
			cand = w->prev[cp & TINYC_WMASK];
		}

		if (best_len >= TINYC_MIN_MATCH) {
			int li = len_index(best_len);
			int di = dist_index(best_dist);

			bw_fixed_lit(&b, 257u + (unsigned)li);
			if (len_extra[li])
				bw_bits(&b, best_len - len_base[li], len_extra[li]);
			bw_code(&b, (unsigned)di, 5);
			if (dist_extra[di])
				bw_bits(&b, best_dist - dist_base[di], dist_extra[di]);

			for (i = 0; i < best_len && p + i + 2 < total; i++)
				chain_insert(w, dict, dict_len, in, p + i);
			p += best_len;
		} else {
			bw_fixed_lit(&b, win_byte(dict, dict_len, in, p));
			if (p + 2 < total)
				chain_insert(w, dict, dict_len, in, p);
			p++;
		}
	}

	if (b.over)
		return 0;
	bw_fixed_lit(&b, 256); /* end of block */
	if (b.over)
		return 0;
	bw_align(&b);
	return b.over ? 0 : b.n;
}

static size_t emit_stored(const u8 *in, size_t in_len, u8 *out,
			  size_t out_max)
{
	struct bw b = { out, out_max, 0, 0, 0, 0 };
	size_t off = 0;

	do {
		size_t n = in_len - off;
		int last;

		if (n > 65535u)
			n = 65535u;
		last = (off + n == in_len);
		bw_bits(&b, last ? 1u : 0u, 1);
		bw_bits(&b, 0, 2); /* BTYPE = 00, stored */
		bw_align(&b);
		if (b.over || b.n + 4 + n > b.cap)
			return 0;
		b.out[b.n++] = (u8)(n & 0xffu);
		b.out[b.n++] = (u8)(n >> 8);
		b.out[b.n++] = (u8)(~n & 0xffu);
		b.out[b.n++] = (u8)((~n >> 8) & 0xffu);
		memcpy(b.out + b.n, in + off, n);
		b.n += n;
		off += n;
	} while (off < in_len);

	return b.n;
}

/* What a stored encoding of in_len bytes costs: one header byte plus LEN and
 * NLEN per block.  Computed rather than encoded, because encoding it would
 * overwrite whatever the other path just wrote into `out`. */
static size_t stored_size(size_t in_len)
{
	return in_len + 5 * (in_len / 65535u + 1);
}

size_t tinyc_deflate(const u8 *dict, size_t dict_len, const u8 *in,
		     size_t in_len, u8 *out, size_t out_max, void *work)
{
	size_t n_fixed, n_stored;
	struct tinyc_work *w = work;

	if (!out || !work || (dict_len && !dict) || (in_len && !in))
		return 0;
	if (dict_len + in_len > TINYC_WINDOW)
		return 0;
	if (in_len == 0)
		return 0;

	/* Encode once, then encode again only if the other path is smaller.  The
	 * two must not both write into `out`: the second would overwrite the
	 * first while the caller was told the first one's length. */
	n_fixed = emit_fixed(dict, dict_len, in, in_len, out, out_max, w);
	n_stored = stored_size(in_len);

	if (n_fixed != 0 && n_fixed <= n_stored)
		return n_fixed;

	n_stored = emit_stored(in, in_len, out, out_max);

	return n_stored;
}
