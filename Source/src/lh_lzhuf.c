/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_lzhuf.c - Classic lh.library LhEncode / LhDecode (LZHUF).
 *
 * Algorithm: LZSS + adaptive Huffman (Haruyasu Yoshizaki / Haruhiko Okumura
 * LZHUF).  Holger Krekel & Olaf Barthel's lh.library v1.0
 * implements this codec for the original four LVOs.
 *
 * Wire format (matches LhEncode/LhDecode demos on the Fish disk):
 *   - LhEncode writes a pure LZHUF bitstream (no leading size word).
 *   - LhDecode expects lh_DstSize = original uncompressed length.
 *   - Demo tools wrap files with an optional 0xFFxxxxxx size ID outside
 *     the library call.
 *
 * Encode work area lives in lh_Aux (~40KB).  Decode uses lh_Aux for the
 * Huffman tables (~4.5KB) and a stack ring buffer.
 */

#include "lh_internal.h"

#include <string.h>

/* LZSS */
#define LHZ_N           4096
#define LHZ_F           60
#define LHZ_THRESHOLD   2
#define LHZ_NIL         LHZ_N

/* Adaptive Huffman */
#define LHZ_N_CHAR      (256 - LHZ_THRESHOLD + LHZ_F)  /* 314 */
#define LHZ_T           (LHZ_N_CHAR * 2 - 1)           /* 627 */
#define LHZ_R           (LHZ_T - 1)                    /* 626 */
#define LHZ_MAX_FREQ    0x8000

typedef struct lh_lzhuf_enc {
    unsigned short freq[LHZ_T + 1];
    unsigned short prnt[LHZ_T + LHZ_N_CHAR];
    unsigned short son[LHZ_T];
    unsigned char text_buf[LHZ_N + LHZ_F - 1];
    unsigned short lson[LHZ_N + 1];
    unsigned short rson[LHZ_N + 257];
    unsigned short dad[LHZ_N + 1];
    unsigned short match_position;
    unsigned short match_length;
    unsigned short putbuf;
    unsigned char putlen;
    unsigned char *out;
    unsigned long out_cap;
    unsigned long out_pos;
} lh_lzhuf_enc;

typedef struct lh_lzhuf_dec {
    unsigned short freq[LHZ_T + 1];
    unsigned short prnt[LHZ_T + LHZ_N_CHAR];
    unsigned short son[LHZ_T];
    unsigned short getbuf;
    unsigned char getlen;
    const unsigned char *in;
    unsigned long in_len;
    unsigned long in_pos;
} lh_lzhuf_dec;

/* Upper 6 bits of match position - encode tables */
static const unsigned char lhz_p_len[64] = {
    0x03, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08
};

static const unsigned char lhz_p_code[64] = {
    0x00, 0x20, 0x30, 0x40, 0x50, 0x58, 0x60, 0x68,
    0x70, 0x78, 0x80, 0x88, 0x90, 0x94, 0x98, 0x9C,
    0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC,
    0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE,
    0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE,
    0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
};

/* Decode tables for upper 6 bits of position */
static const unsigned char lhz_d_code[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
    0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D,
    0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F,
    0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11,
    0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
    0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15,
    0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x17,
    0x18, 0x18, 0x19, 0x19, 0x1A, 0x1A, 0x1B, 0x1B,
    0x1C, 0x1C, 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F,
    0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23,
    0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
    0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B,
    0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

static const unsigned char lhz_d_len[256] = {
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08
};

static void lhz_init_tree(lh_lzhuf_enc *e)
{
    int i;

    for (i = LHZ_N + 1; i <= LHZ_N + 256; i++) {
        e->rson[i] = LHZ_NIL;
    }
    for (i = 0; i < LHZ_N; i++) {
        e->dad[i] = LHZ_NIL;
    }
}

static void lhz_insert_node(lh_lzhuf_enc *e, int r)
{
    int i;
    int p;
    int cmp;
    unsigned char *key;
    unsigned int c;

    cmp = 1;
    key = &e->text_buf[r];
    p = LHZ_N + 1 + key[0];
    e->rson[r] = LHZ_NIL;
    e->lson[r] = LHZ_NIL;
    e->match_length = 0;
    for (;;) {
        if (cmp >= 0) {
            if (e->rson[p] != LHZ_NIL) {
                p = e->rson[p];
            } else {
                e->rson[p] = (unsigned short)r;
                e->dad[r] = (unsigned short)p;
                return;
            }
        } else {
            if (e->lson[p] != LHZ_NIL) {
                p = e->lson[p];
            } else {
                e->lson[p] = (unsigned short)r;
                e->dad[r] = (unsigned short)p;
                return;
            }
        }
        for (i = 1; i < LHZ_F; i++) {
            cmp = (int)key[i] - (int)e->text_buf[p + i];
            if (cmp != 0) {
                break;
            }
        }
        if (i > LHZ_THRESHOLD) {
            if (i > (int)e->match_length) {
                e->match_position = (unsigned short)(((r - p) & (LHZ_N - 1)) - 1);
                e->match_length = (unsigned short)i;
                if (e->match_length >= LHZ_F) {
                    break;
                }
            }
            if (i == (int)e->match_length) {
                c = (unsigned int)(((r - p) & (LHZ_N - 1)) - 1);
                if (c < (unsigned int)e->match_position) {
                    e->match_position = (unsigned short)c;
                }
            }
        }
    }
    e->dad[r] = e->dad[p];
    e->lson[r] = e->lson[p];
    e->rson[r] = e->rson[p];
    e->dad[e->lson[p]] = (unsigned short)r;
    e->dad[e->rson[p]] = (unsigned short)r;
    if (e->rson[e->dad[p]] == (unsigned short)p) {
        e->rson[e->dad[p]] = (unsigned short)r;
    } else {
        e->lson[e->dad[p]] = (unsigned short)r;
    }
    e->dad[p] = LHZ_NIL;
}

static void lhz_delete_node(lh_lzhuf_enc *e, int p)
{
    int q;

    if (e->dad[p] == LHZ_NIL) {
        return;
    }
    if (e->rson[p] == LHZ_NIL) {
        q = e->lson[p];
    } else if (e->lson[p] == LHZ_NIL) {
        q = e->rson[p];
    } else {
        q = e->lson[p];
        if (e->rson[q] != LHZ_NIL) {
            do {
                q = e->rson[q];
            } while (e->rson[q] != LHZ_NIL);
            e->rson[e->dad[q]] = e->lson[q];
            e->dad[e->lson[q]] = e->dad[q];
            e->lson[q] = e->lson[p];
            e->dad[e->lson[p]] = (unsigned short)q;
        }
        e->rson[q] = e->rson[p];
        e->dad[e->rson[p]] = (unsigned short)q;
    }
    e->dad[q] = e->dad[p];
    if (e->rson[e->dad[p]] == (unsigned short)p) {
        e->rson[e->dad[p]] = (unsigned short)q;
    } else {
        e->lson[e->dad[p]] = (unsigned short)q;
    }
    e->dad[p] = LHZ_NIL;
}

static void lhz_start_huff_enc(lh_lzhuf_enc *e)
{
    unsigned short i;
    unsigned short j;

    for (i = 0; i < LHZ_N_CHAR; i++) {
        e->freq[i] = 1;
        e->son[i] = (unsigned short)(i + LHZ_T);
        e->prnt[i + LHZ_T] = i;
    }
    i = 0;
    j = LHZ_N_CHAR;
    while (j <= LHZ_R) {
        e->freq[j] = (unsigned short)(e->freq[i] + e->freq[i + 1]);
        e->son[j] = i;
        e->prnt[i] = j;
        e->prnt[i + 1] = j;
        i = (unsigned short)(i + 2);
        j++;
    }
    e->freq[LHZ_T] = 0xffff;
    e->prnt[LHZ_R] = 0;
}

static void lhz_start_huff_dec(lh_lzhuf_dec *d)
{
    unsigned short i;
    unsigned short j;

    for (i = 0; i < LHZ_N_CHAR; i++) {
        d->freq[i] = 1;
        d->son[i] = (unsigned short)(i + LHZ_T);
        d->prnt[i + LHZ_T] = i;
    }
    i = 0;
    j = LHZ_N_CHAR;
    while (j <= LHZ_R) {
        d->freq[j] = (unsigned short)(d->freq[i] + d->freq[i + 1]);
        d->son[j] = i;
        d->prnt[i] = j;
        d->prnt[i + 1] = j;
        i = (unsigned short)(i + 2);
        j++;
    }
    d->freq[LHZ_T] = 0xffff;
    d->prnt[LHZ_R] = 0;
}

static void lhz_move_words(unsigned short *dst, const unsigned short *src, unsigned n)
{
    unsigned i;

    if (dst == src || n == 0) {
        return;
    }
    if (dst < src) {
        for (i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    } else {
        i = n;
        while (i > 0) {
            i--;
            dst[i] = src[i];
        }
    }
}

static void lhz_reconst_enc(lh_lzhuf_enc *e)
{
    int i;
    int j;
    int k;
    unsigned f;
    unsigned l;

    j = 0;
    for (i = 0; i < LHZ_T; i++) {
        if (e->son[i] >= LHZ_T) {
            e->freq[j] = (unsigned short)((e->freq[i] + 1) / 2);
            e->son[j] = e->son[i];
            j++;
        }
    }
    for (i = 0, j = LHZ_N_CHAR; j < LHZ_T; i += 2, j++) {
        k = i + 1;
        f = (unsigned)e->freq[i] + (unsigned)e->freq[k];
        e->freq[j] = (unsigned short)f;
        k = j - 1;
        while (f < (unsigned)e->freq[k]) {
            k--;
        }
        k++;
        l = (unsigned)(j - k) * 2u;
        lhz_move_words(&e->freq[k + 1], &e->freq[k], l / 2u);
        e->freq[k] = (unsigned short)f;
        lhz_move_words(&e->son[k + 1], &e->son[k], l / 2u);
        e->son[k] = (unsigned short)i;
    }
    for (i = 0; i < LHZ_T; i++) {
        k = e->son[i];
        if (k >= LHZ_T) {
            e->prnt[k] = (unsigned short)i;
        } else {
            e->prnt[k] = (unsigned short)i;
            e->prnt[k + 1] = (unsigned short)i;
        }
    }
}

static void lhz_reconst_dec(lh_lzhuf_dec *d)
{
    int i;
    int j;
    int k;
    unsigned f;
    unsigned l;

    j = 0;
    for (i = 0; i < LHZ_T; i++) {
        if (d->son[i] >= LHZ_T) {
            d->freq[j] = (unsigned short)((d->freq[i] + 1) / 2);
            d->son[j] = d->son[i];
            j++;
        }
    }
    for (i = 0, j = LHZ_N_CHAR; j < LHZ_T; i += 2, j++) {
        k = i + 1;
        f = (unsigned)d->freq[i] + (unsigned)d->freq[k];
        d->freq[j] = (unsigned short)f;
        k = j - 1;
        while (f < (unsigned)d->freq[k]) {
            k--;
        }
        k++;
        l = (unsigned)(j - k) * 2u;
        lhz_move_words(&d->freq[k + 1], &d->freq[k], l / 2u);
        d->freq[k] = (unsigned short)f;
        lhz_move_words(&d->son[k + 1], &d->son[k], l / 2u);
        d->son[k] = (unsigned short)i;
    }
    for (i = 0; i < LHZ_T; i++) {
        k = d->son[i];
        if (k >= LHZ_T) {
            d->prnt[k] = (unsigned short)i;
        } else {
            d->prnt[k] = (unsigned short)i;
            d->prnt[k + 1] = (unsigned short)i;
        }
    }
}

static void lhz_update_enc(lh_lzhuf_enc *e, unsigned int c)
{
    unsigned int i;
    unsigned int j;
    unsigned int k;
    unsigned int l;

    if (e->freq[LHZ_R] == LHZ_MAX_FREQ) {
        lhz_reconst_enc(e);
    }
    c = e->prnt[c + LHZ_T];
    do {
        k = ++e->freq[c];
        l = c + 1;
        if (k > e->freq[l]) {
            while (k > e->freq[++l]) {
                /* find insert point */
            }
            l--;
            e->freq[c] = e->freq[l];
            e->freq[l] = (unsigned short)k;

            i = e->son[c];
            e->prnt[i] = (unsigned short)l;
            if (i < LHZ_T) {
                e->prnt[i + 1] = (unsigned short)l;
            }

            j = e->son[l];
            e->son[l] = (unsigned short)i;

            e->prnt[j] = (unsigned short)c;
            if (j < LHZ_T) {
                e->prnt[j + 1] = (unsigned short)c;
            }
            e->son[c] = (unsigned short)j;

            c = l;
        }
        c = e->prnt[c];
    } while (c != 0);
}

static void lhz_update_dec(lh_lzhuf_dec *d, unsigned int c)
{
    unsigned int i;
    unsigned int j;
    unsigned int k;
    unsigned int l;

    if (d->freq[LHZ_R] == LHZ_MAX_FREQ) {
        lhz_reconst_dec(d);
    }
    c = d->prnt[c + LHZ_T];
    do {
        k = ++d->freq[c];
        l = c + 1;
        if (k > d->freq[l]) {
            while (k > d->freq[++l]) {
                /* find insert point */
            }
            l--;
            d->freq[c] = d->freq[l];
            d->freq[l] = (unsigned short)k;

            i = d->son[c];
            d->prnt[i] = (unsigned short)l;
            if (i < LHZ_T) {
                d->prnt[i + 1] = (unsigned short)l;
            }

            j = d->son[l];
            d->son[l] = (unsigned short)i;

            d->prnt[j] = (unsigned short)c;
            if (j < LHZ_T) {
                d->prnt[j + 1] = (unsigned short)c;
            }
            d->son[c] = (unsigned short)j;

            c = l;
        }
        c = d->prnt[c];
    } while (c != 0);
}

static int lhz_putcode(lh_lzhuf_enc *e, int l, unsigned int c)
{
    e->putbuf = (unsigned short)(e->putbuf | (c >> e->putlen));
    e->putlen = (unsigned char)(e->putlen + l);
    if (e->putlen >= 8) {
        if (e->out_pos >= e->out_cap) {
            return 0;
        }
        e->out[e->out_pos++] = (unsigned char)(e->putbuf >> 8);
        e->putlen = (unsigned char)(e->putlen - 8);
        if (e->putlen >= 8) {
            if (e->out_pos >= e->out_cap) {
                return 0;
            }
            e->out[e->out_pos++] = (unsigned char)e->putbuf;
            e->putlen = (unsigned char)(e->putlen - 8);
            e->putbuf = (unsigned short)(c << (l - e->putlen));
        } else {
            e->putbuf = (unsigned short)(e->putbuf << 8);
        }
    }
    return 1;
}

static int lhz_encode_char(lh_lzhuf_enc *e, unsigned c)
{
    unsigned i;
    int j;
    int k;

    i = 0;
    j = 0;
    k = e->prnt[c + LHZ_T];
    do {
        i >>= 1;
        if (k & 1) {
            i += 0x8000u;
        }
        j++;
        k = e->prnt[k];
    } while (k != LHZ_R);
    if (!lhz_putcode(e, j, i)) {
        return 0;
    }
    lhz_update_enc(e, c);
    return 1;
}

static int lhz_encode_position(lh_lzhuf_enc *e, unsigned c)
{
    unsigned i;

    i = c >> 6;
    if (!lhz_putcode(e, (int)lhz_p_len[i], (unsigned)lhz_p_code[i] << 8)) {
        return 0;
    }
    if (!lhz_putcode(e, 6, (c & 0x3fu) << 10)) {
        return 0;
    }
    return 1;
}

static int lhz_encode_end(lh_lzhuf_enc *e)
{
    if (e->putlen) {
        if (e->out_pos >= e->out_cap) {
            return 0;
        }
        e->out[e->out_pos++] = (unsigned char)(e->putbuf >> 8);
    }
    return 1;
}

static unsigned short lhz_getbit(lh_lzhuf_dec *d)
{
    unsigned short i;

    while (d->getlen <= 8) {
        if (d->in_pos < d->in_len) {
            i = d->in[d->in_pos++];
        } else {
            i = 0;
        }
        d->getbuf = (unsigned short)(d->getbuf | (i << (8 - d->getlen)));
        d->getlen = (unsigned char)(d->getlen + 8);
    }
    i = d->getbuf;
    d->getbuf = (unsigned short)(d->getbuf << 1);
    d->getlen--;
    return (unsigned short)(i >> 15);
}

static unsigned short lhz_getbyte(lh_lzhuf_dec *d)
{
    unsigned short i;

    while (d->getlen <= 8) {
        if (d->in_pos < d->in_len) {
            i = d->in[d->in_pos++];
        } else {
            i = 0;
        }
        d->getbuf = (unsigned short)(d->getbuf | (i << (8 - d->getlen)));
        d->getlen = (unsigned char)(d->getlen + 8);
    }
    i = d->getbuf;
    d->getbuf = (unsigned short)(d->getbuf << 8);
    d->getlen = (unsigned char)(d->getlen - 8);
    return (unsigned short)(i >> 8);
}

static unsigned int lhz_decode_char(lh_lzhuf_dec *d)
{
    unsigned int c;

    c = d->son[LHZ_R];
    while (c < LHZ_T) {
        c += lhz_getbit(d);
        c = d->son[c];
    }
    c -= LHZ_T;
    lhz_update_dec(d, c);
    return c;
}

static unsigned int lhz_decode_position(lh_lzhuf_dec *d)
{
    unsigned int i;
    unsigned int j;
    unsigned int c;

    i = lhz_getbyte(d);
    c = (unsigned int)lhz_d_code[i] << 6;
    j = lhz_d_len[i];
    j -= 2;
    while (j--) {
        i = (i << 1) + lhz_getbit(d);
    }
    return c | (i & 0x3fu);
}

unsigned long lh_lzhuf_encode(LhBuffer *buf)
{
    lh_lzhuf_enc *e;
    const unsigned char *input;
    unsigned long inlen;
    unsigned long offset;
    int i;
    int c;
    int len;
    int r;
    int s;
    int last_match_length;

    if (!buf || !buf->lh_Src || !buf->lh_Dst || !buf->lh_Aux) {
        return 0;
    }
    if (buf->lh_SrcSize == 0) {
        return 0;
    }
    if (buf->lh_AuxSize < (unsigned long)sizeof(lh_lzhuf_enc)) {
        return 0;
    }
    inlen = buf->lh_SrcSize;
    if (buf->lh_DstSize < inlen + LH_ENCODE_EXTRA(inlen)) {
        return 0;
    }

    e = (lh_lzhuf_enc *)buf->lh_Aux;
    memset(e, 0, sizeof(*e));
    e->out = (unsigned char *)buf->lh_Dst;
    e->out_cap = buf->lh_DstSize;
    e->out_pos = 0;
    e->putbuf = 0;
    e->putlen = 0;

    input = (const unsigned char *)buf->lh_Src;
    offset = 0;

    lhz_start_huff_enc(e);
    lhz_init_tree(e);

    s = 0;
    r = LHZ_N - LHZ_F;
    for (i = s; i < r; i++) {
        e->text_buf[i] = (unsigned char)' ';
    }
    for (len = 0; len < LHZ_F && offset < inlen; len++) {
        e->text_buf[r + len] = input[offset++];
    }
    for (i = 1; i <= LHZ_F; i++) {
        lhz_insert_node(e, r - i);
    }
    lhz_insert_node(e, r);

    do {
        if (e->match_length > (unsigned short)len) {
            e->match_length = (unsigned short)len;
        }
        if (e->match_length <= LHZ_THRESHOLD) {
            e->match_length = 1;
            if (!lhz_encode_char(e, e->text_buf[r])) {
                return 0;
            }
        } else {
            if (!lhz_encode_char(e, 255 - LHZ_THRESHOLD + e->match_length)) {
                return 0;
            }
            if (!lhz_encode_position(e, e->match_position)) {
                return 0;
            }
        }
        last_match_length = (int)e->match_length;
        for (i = 0; i < last_match_length && offset < inlen; i++) {
            c = input[offset++];
            lhz_delete_node(e, s);
            e->text_buf[s] = (unsigned char)c;
            if (s < LHZ_F - 1) {
                e->text_buf[s + LHZ_N] = (unsigned char)c;
            }
            s = (s + 1) & (LHZ_N - 1);
            r = (r + 1) & (LHZ_N - 1);
            lhz_insert_node(e, r);
        }
        while (i++ < last_match_length) {
            lhz_delete_node(e, s);
            s = (s + 1) & (LHZ_N - 1);
            r = (r + 1) & (LHZ_N - 1);
            if (--len) {
                lhz_insert_node(e, r);
            }
        }
    } while (len > 0);

    if (!lhz_encode_end(e)) {
        return 0;
    }
    buf->lh_DstSize = e->out_pos;
    return e->out_pos;
}

unsigned long lh_lzhuf_decode(LhBuffer *buf)
{
    lh_lzhuf_dec *d;
    unsigned char text_buf[LHZ_N + LHZ_F - 1];
    unsigned char *out;
    unsigned long textsize;
    unsigned long count;
    unsigned int c;
    unsigned int i;
    unsigned int j;
    unsigned int k;
    unsigned int r;

    if (!buf || !buf->lh_Src || !buf->lh_Dst || !buf->lh_Aux) {
        return 0;
    }
    if (buf->lh_SrcSize == 0 || buf->lh_DstSize == 0) {
        return 0;
    }
    if (buf->lh_AuxSize < (unsigned long)sizeof(lh_lzhuf_dec)) {
        return 0;
    }

    d = (lh_lzhuf_dec *)buf->lh_Aux;
    memset(d, 0, sizeof(*d));
    d->in = (const unsigned char *)buf->lh_Src;
    d->in_len = buf->lh_SrcSize;
    d->in_pos = 0;
    d->getbuf = 0;
    d->getlen = 0;

    textsize = buf->lh_DstSize;
    out = (unsigned char *)buf->lh_Dst;

    lhz_start_huff_dec(d);
    for (i = 0; i < (unsigned int)(LHZ_N - LHZ_F); i++) {
        text_buf[i] = (unsigned char)' ';
    }
    r = (unsigned int)(LHZ_N - LHZ_F);

    for (count = 0; count < textsize; ) {
        c = lhz_decode_char(d);
        if (c < 256) {
            out[count] = (unsigned char)c;
            text_buf[r++] = (unsigned char)c;
            r &= (unsigned int)(LHZ_N - 1);
            count++;
        } else {
            i = (r - lhz_decode_position(d) - 1) & (unsigned int)(LHZ_N - 1);
            j = c - 255 + LHZ_THRESHOLD;
            for (k = 0; k < j && count < textsize; k++) {
                c = text_buf[(i + k) & (unsigned int)(LHZ_N - 1)];
                out[count] = (unsigned char)c;
                text_buf[r++] = (unsigned char)c;
                r &= (unsigned int)(LHZ_N - 1);
                count++;
            }
        }
    }

    buf->lh_DstSize = count;
    return count;
}
