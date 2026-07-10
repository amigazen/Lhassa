/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_lh5dec.c - LH4/LH5/LH6/LH7 decompression (static Huffman + sliding dict).
 *
 * Same block format; dictionary size / PBIT / NP differ:
 *   lh4: 4 KiB  (dicbit 12, pbit 4, np 13)
 *   lh5: 8 KiB  (dicbit 13, pbit 4, np 14)
 *   lh6: 32 KiB (dicbit 15, pbit 5, np 16)
 *   lh7: 64 KiB (dicbit 16, pbit 5, np 17)
 *   lhx: UNLHA32 (dicbit 20, pbit 5, np 31) — 1 MiB history ring
 *
 * Decoder state is malloc'd — the Huffman tables alone are ~13 KiB and
 * must not live on a typical Amiga process stack.
 */

#include "lh_internal.h"

#include <limits.h>
#include <string.h>

#define LH5_MAXMATCH      256
#define LH5_THRESHOLD     3
#define LH5_NC            (UCHAR_MAX + LH5_MAXMATCH + 2 - LH5_THRESHOLD)
#define LH5_CBIT          9
#define LH5_NT            (16 + 3)
#define LH5_TBIT          5
#define LH5_NPT           0x80

struct lh5_dec {
    const unsigned char *in;
    unsigned long in_size;
    unsigned long in_pos;
    unsigned long compsize;
    unsigned short bitbuf;
    unsigned char subbitbuf;
    unsigned char bitcount;
    unsigned short blocksize;
    short np;
    short pbit;
    unsigned short left[2 * LH5_NC - 1];
    unsigned short right[2 * LH5_NC - 1];
    unsigned char c_len[LH5_NC];
    unsigned char pt_len[LH5_NPT];
    unsigned short c_table[4096];
    unsigned short pt_table[256];
};

static void lh5_fillbuf(struct lh5_dec *d, unsigned char n)
{
    while (n > d->bitcount) {
        n = (unsigned char)(n - d->bitcount);
        d->bitbuf = (unsigned short)((d->bitbuf << d->bitcount)
            + (d->subbitbuf >> (CHAR_BIT - d->bitcount)));
        if (d->compsize != 0) {
            d->compsize--;
            if (d->in_pos < d->in_size) {
                d->subbitbuf = d->in[d->in_pos++];
            } else {
                d->subbitbuf = 0;
            }
        } else {
            d->subbitbuf = 0;
        }
        d->bitcount = CHAR_BIT;
    }
    d->bitcount = (unsigned char)(d->bitcount - n);
    d->bitbuf = (unsigned short)((d->bitbuf << n)
        + (d->subbitbuf >> (CHAR_BIT - n)));
    d->subbitbuf = (unsigned char)(d->subbitbuf << n);
}

static unsigned short lh5_getbits(struct lh5_dec *d, unsigned char n)
{
    unsigned short x;

    x = (unsigned short)(d->bitbuf >> (2 * CHAR_BIT - n));
    lh5_fillbuf(d, n);
    return x;
}

static void lh5_init_getbits(struct lh5_dec *d)
{
    d->bitbuf = 0;
    d->subbitbuf = 0;
    d->bitcount = 0;
    lh5_fillbuf(d, (unsigned char)(2 * CHAR_BIT));
}

static void lh5_make_table(
    struct lh5_dec *d,
    short nchar,
    unsigned char *bitlen,
    short tablebits,
    unsigned short *table)
{
    unsigned short count[17];
    unsigned short weight[17];
    unsigned short start[17];
    unsigned short total;
    unsigned int i;
    int j;
    int k;
    int l;
    int m;
    int n;
    int avail;
    unsigned short *p;

    avail = nchar;
    for (i = 1; i <= 16; i++) {
        count[i] = 0;
        weight[i] = (unsigned short)(1 << (16 - i));
    }
    for (i = 0; i < (unsigned int)nchar; i++) {
        count[bitlen[i]]++;
    }
    total = 0;
    for (i = 1; i <= 16; i++) {
        start[i] = total;
        total = (unsigned short)(total + weight[i] * count[i]);
    }
    if ((total & 0xffffu) != 0) {
        return;
    }
    m = 16 - tablebits;
    for (i = 1; i <= (unsigned int)tablebits; i++) {
        start[i] = (unsigned short)(start[i] >> m);
        weight[i] = (unsigned short)(weight[i] >> m);
    }
    j = (int)(start[tablebits + 1] >> m);
    k = 1 << tablebits;
    if (j != 0) {
        for (i = (unsigned int)j; i < (unsigned int)k; i++) {
            table[i] = 0;
        }
    }
    for (j = 0; j < nchar; j++) {
        k = bitlen[j];
        if (k == 0) {
            continue;
        }
        l = (int)start[k] + (int)weight[k];
        if (k <= tablebits) {
            for (i = start[k]; i < (unsigned int)l; i++) {
                table[i] = (unsigned short)j;
            }
        } else {
            p = &table[(i = start[k]) >> m];
            i <<= tablebits;
            n = k - tablebits;
            while (--n >= 0) {
                if (*p == 0) {
                    d->right[avail] = 0;
                    d->left[avail] = 0;
                    *p = (unsigned short)avail++;
                }
                if (i & 0x8000u) {
                    p = &d->right[*p];
                } else {
                    p = &d->left[*p];
                }
                i <<= 1;
            }
            *p = (unsigned short)j;
        }
        start[k] = (unsigned short)l;
    }
}

static void lh5_read_pt_len(
    struct lh5_dec *d,
    short nn,
    short nbit,
    short i_special)
{
    short i;
    short c;
    short n;
    unsigned short mask;

    n = (short)lh5_getbits(d, (unsigned char)nbit);
    if (n == 0) {
        c = (short)lh5_getbits(d, (unsigned char)nbit);
        for (i = 0; i < nn; i++) {
            d->pt_len[i] = 0;
        }
        for (i = 0; i < 256; i++) {
            d->pt_table[i] = (unsigned short)c;
        }
    } else {
        i = 0;
        while (i < n) {
            c = (short)(d->bitbuf >> (16 - 3));
            if (c == 7) {
                mask = (unsigned short)(1 << (16 - 4));
                while (mask & d->bitbuf) {
                    mask = (unsigned short)(mask >> 1);
                    c++;
                }
            }
            lh5_fillbuf(d, (unsigned char)((c < 7) ? 3 : c - 3));
            d->pt_len[i++] = (unsigned char)c;
            if (i == i_special) {
                c = (short)lh5_getbits(d, 2);
                while (--c >= 0) {
                    d->pt_len[i++] = 0;
                }
            }
        }
        while (i < nn) {
            d->pt_len[i++] = 0;
        }
        lh5_make_table(d, nn, d->pt_len, 8, d->pt_table);
    }
}

static void lh5_read_c_len(struct lh5_dec *d)
{
    short i;
    short c;
    short n;
    unsigned short mask;

    n = (short)lh5_getbits(d, LH5_CBIT);
    if (n == 0) {
        c = (short)lh5_getbits(d, LH5_CBIT);
        for (i = 0; i < LH5_NC; i++) {
            d->c_len[i] = 0;
        }
        for (i = 0; i < 4096; i++) {
            d->c_table[i] = (unsigned short)c;
        }
    } else {
        i = 0;
        while (i < n) {
            c = (short)d->pt_table[d->bitbuf >> (16 - 8)];
            if (c >= LH5_NT) {
                mask = (unsigned short)(1 << (16 - 9));
                do {
                    if (d->bitbuf & mask) {
                        c = (short)d->right[c];
                    } else {
                        c = (short)d->left[c];
                    }
                    mask = (unsigned short)(mask >> 1);
                } while (c >= LH5_NT);
            }
            lh5_fillbuf(d, d->pt_len[c]);
            if (c <= 2) {
                if (c == 0) {
                    c = 1;
                } else if (c == 1) {
                    c = (short)(lh5_getbits(d, 4) + 3);
                } else {
                    c = (short)(lh5_getbits(d, LH5_CBIT) + 20);
                }
                while (--c >= 0) {
                    d->c_len[i++] = 0;
                }
            } else {
                d->c_len[i++] = (unsigned char)(c - 2);
            }
        }
        while (i < LH5_NC) {
            d->c_len[i++] = 0;
        }
        lh5_make_table(d, LH5_NC, d->c_len, 12, d->c_table);
    }
}

static unsigned short lh5_decode_c(struct lh5_dec *d)
{
    unsigned short j;
    unsigned short mask;

    if (d->blocksize == 0) {
        d->blocksize = lh5_getbits(d, 16);
        lh5_read_pt_len(d, LH5_NT, LH5_TBIT, 3);
        lh5_read_c_len(d);
        lh5_read_pt_len(d, d->np, d->pbit, -1);
    }
    d->blocksize--;
    j = d->c_table[d->bitbuf >> 4];
    if (j < LH5_NC) {
        lh5_fillbuf(d, d->c_len[j]);
    } else {
        lh5_fillbuf(d, 12);
        mask = (unsigned short)(1 << (16 - 1));
        do {
            if (d->bitbuf & mask) {
                j = d->right[j];
            } else {
                j = d->left[j];
            }
            mask = (unsigned short)(mask >> 1);
        } while (j >= LH5_NC);
        lh5_fillbuf(d, (unsigned char)(d->c_len[j] - 12));
    }
    return j;
}

static unsigned short lh5_decode_p(struct lh5_dec *d)
{
    unsigned short j;
    unsigned short mask;

    j = d->pt_table[d->bitbuf >> (16 - 8)];
    if (j < (unsigned short)d->np) {
        lh5_fillbuf(d, d->pt_len[j]);
    } else {
        lh5_fillbuf(d, 8);
        mask = (unsigned short)(1 << (16 - 1));
        do {
            if (d->bitbuf & mask) {
                j = d->right[j];
            } else {
                j = d->left[j];
            }
            mask = (unsigned short)(mask >> 1);
        } while (j >= (unsigned short)d->np);
        lh5_fillbuf(d, (unsigned char)(d->pt_len[j] - 8));
    }
    if (j != 0) {
        j = (unsigned short)((1 << (j - 1)) + lh5_getbits(d, (unsigned char)(j - 1)));
    }
    return j;
}

/*
 * Shared LH4..LH7 / LHX decoder.  dicbit selects window size (2^dicbit);
 * pbit is the bit width used when reading the position-tree length count;
 * np is the number of position codes (usually dicbit+1; LHX uses 31).
 */
static long lh_decompress_lh_new(
    void *inBuf,
    unsigned long inSize,
    void *outBuf,
    unsigned long outSize,
    unsigned int dicbit,
    unsigned int pbit,
    unsigned int np)
{
    struct lh5_dec *dec;
    unsigned char *text;
    unsigned char *out;
    unsigned long dicsiz;
    unsigned long dicsiz1;
    unsigned long count;
    unsigned long loc;
    unsigned long i;
    unsigned long j;
    unsigned long k;
    unsigned long c;
    int offset;
    unsigned char b;
    long rc;

    if (!inBuf || !outBuf || inSize == 0 || outSize == 0) {
        return LH_ERR_INVALID_ARG;
    }
    if (dicbit < 12 || dicbit > 20 || pbit < 4 || pbit > 5) {
        return LH_ERR_INVALID_ARG;
    }
    if (np == 0 || np > LH5_NPT) {
        return LH_ERR_INVALID_ARG;
    }

    dicsiz = 1UL << dicbit;
    text = (unsigned char *)malloc(dicsiz);
    if (!text) {
        return LH_ERR_NO_MEMORY;
    }
    dec = (struct lh5_dec *)malloc(sizeof(*dec));
    if (!dec) {
        free(text);
        return LH_ERR_NO_MEMORY;
    }

    memset(dec, 0, sizeof(*dec));
    dec->in = (const unsigned char *)inBuf;
    dec->in_size = inSize;
    dec->in_pos = 0;
    dec->compsize = inSize;
    dec->blocksize = 0;
    dec->np = (short)np;
    dec->pbit = (short)pbit;
    lh5_init_getbits(dec);

    memset(text, ' ', dicsiz);
    dicsiz1 = dicsiz - 1;
    offset = 0x100 - LH5_THRESHOLD;
    count = 0;
    loc = 0;
    out = (unsigned char *)outBuf;
    rc = LH_OK;

    while (count < outSize) {
        c = lh5_decode_c(dec);
        if (c <= UCHAR_MAX) {
            text[loc++] = (unsigned char)c;
            if (loc == dicsiz) {
                loc = 0;
            }
            out[count++] = (unsigned char)c;
        } else {
            j = c - (unsigned long)offset;
            i = (loc - lh5_decode_p(dec) - 1) & dicsiz1;
            for (k = 0; k < j; k++) {
                b = text[(i + k) & dicsiz1];
                text[loc++] = b;
                if (loc == dicsiz) {
                    loc = 0;
                }
                out[count++] = b;
                if (count >= outSize) {
                    break;
                }
            }
        }
    }

    free(dec);
    free(text);
    return rc;
}

long lh_decompress_lh4(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 12, 4, 13);
}

long lh_decompress_lh5(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 13, 4, 14);
}

long lh_decompress_lh6(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 15, 5, 16);
}

long lh_decompress_lh7(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 16, 5, 17);
}

long lh_decompress_lhx(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    /* UNLHA32 -lhx-: 1 MiB ring, OFFSET_BITS=5 → up to 31 position codes. */
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 20, 5, 31);
}
