/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_buffer.c - lh.library-compatible in-memory buffer API.
 */

#include "lh_internal.h"

struct lh_buffer_wrap {
    LhBuffer pub;
    int only_decode;
};

LhBuffer *lh_create_buffer(int only_decode)
{
    struct lh_buffer_wrap *w;
    size_t aux_size;

    w = (struct lh_buffer_wrap *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    aux_size = only_decode ? LH_BUF_DECODE_AUX : LH_BUF_ENCODE_AUX;
    w->pub.lh_Aux = malloc(aux_size);
    if (!w->pub.lh_Aux) {
        free(w);
        return NULL;
    }
    w->pub.lh_AuxSize = (unsigned long)aux_size;
    w->only_decode = only_decode;
    return &w->pub;
}

void lh_delete_buffer(LhBuffer *buf)
{
    struct lh_buffer_wrap *w;

    if (!buf) {
        return;
    }
    w = (struct lh_buffer_wrap *)buf;
    free(w->pub.lh_Aux);
    free(w);
}

unsigned long lh_encode(LhBuffer *buf)
{
    lh_status st;
    size_t out_len;
    unsigned long result;

    if (!buf || !buf->lh_Src || !buf->lh_Dst || buf->lh_SrcSize == 0) {
        return 0;
    }
    out_len = (size_t)buf->lh_DstSize;
    st = lh_compress(LH_METHOD_LH0,
        (const unsigned char *)buf->lh_Src, (size_t)buf->lh_SrcSize,
        (unsigned char *)buf->lh_Dst, &out_len);
    if (st != LH_OK) {
        return 0;
    }
    result = (unsigned long)out_len;
    buf->lh_DstSize = result;
    return result;
}

unsigned long lh_decode(LhBuffer *buf)
{
    lh_status st;
    size_t expected;

    if (!buf || !buf->lh_Src || !buf->lh_Dst || buf->lh_SrcSize == 0) {
        return 0;
    }
    expected = (size_t)buf->lh_DstSize;
    st = lh_decompress(LH_METHOD_LH0,
        (const unsigned char *)buf->lh_Src, (size_t)buf->lh_SrcSize,
        expected,
        (unsigned char *)buf->lh_Dst, expected);
    if (st != LH_OK) {
        return 0;
    }
    return buf->lh_DstSize;
}
