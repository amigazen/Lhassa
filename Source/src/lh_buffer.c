/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_buffer.c - lh.library-compatible in-memory buffer API.
 *
 * LhEncode / LhDecode use classic LZHUF (see lh_lzhuf.c).  CreateBuffer
 * sizes match Lh.doc: 40000 encode aux, 4500 decode-only aux.
 */

#include "lh_internal.h"

#include <stddef.h>

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
    /* buf points at wrap->pub, not at the start of lh_buffer_wrap */
    w = (struct lh_buffer_wrap *)((char *)buf - offsetof(struct lh_buffer_wrap, pub));
    free(w->pub.lh_Aux);
    free(w);
}

unsigned long lh_encode(LhBuffer *buf)
{
    size_t min_dst;

    if (!buf || !buf->lh_Src || !buf->lh_Dst || buf->lh_SrcSize == 0) {
        return 0;
    }
    if (!buf->lh_Aux || buf->lh_AuxSize == 0) {
        return 0;
    }
    min_dst = (size_t)buf->lh_SrcSize + LH_ENCODE_EXTRA((size_t)buf->lh_SrcSize);
    if ((size_t)buf->lh_DstSize < min_dst) {
        return 0;
    }
    /* Adaptive LZH (LZHUF) — original lh.library LhEncode. */
    return lh_lzhuf_encode(buf);
}

unsigned long lh_decode(LhBuffer *buf)
{
    if (!buf || !buf->lh_Src || !buf->lh_Dst || buf->lh_SrcSize == 0) {
        return 0;
    }
    if (buf->lh_DstSize == 0) {
        return 0;
    }
    if (!buf->lh_Aux || buf->lh_AuxSize == 0) {
        return 0;
    }
    /* Adaptive LZH (LZHUF) — original lh.library LhDecode. */
    return lh_lzhuf_decode(buf);
}
