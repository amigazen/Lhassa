/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_error.c - Status strings and method helpers.
 */

#include "lh_internal.h"

const char *lh_status_string(lh_status st)
{
    switch (st) {
    case LH_OK: return "ok";
    case LH_ERR_IO: return "I/O error";
    case LH_ERR_TRUNCATED: return "truncated";
    case LH_ERR_INVALID_ARCHIVE: return "invalid archive";
    case LH_ERR_CRC_MISMATCH: return "CRC mismatch";
    case LH_ERR_BAD_HEADER: return "bad header";
    case LH_ERR_FILENAME_TOO_LONG: return "filename too long";
    case LH_ERR_NO_MEMORY: return "out of memory";
    case LH_ERR_INVALID_ARG: return "invalid argument";
    case LH_ERR_UNSUPPORTED: return "unsupported";
    case LH_ERR_PASSWORD: return "password required";
    case LH_ERR_NOT_FOUND: return "not found";
    case LH_ERR_ABORTED: return "aborted";
    default: return "unknown error";
    }
}

lh_method lh_method_from_string(const char *sig)
{
    if (!sig) {
        return LH_METHOD_LH5;
    }
    if (strncmp(sig, LH_SIG_LH0, LH_SIG_LEN) == 0) return LH_METHOD_LH0;
    if (strncmp(sig, LH_SIG_LH1, LH_SIG_LEN) == 0) return LH_METHOD_LH1;
    if (strncmp(sig, LH_SIG_LH2, LH_SIG_LEN) == 0) return LH_METHOD_LH2;
    if (strncmp(sig, LH_SIG_LH3, LH_SIG_LEN) == 0) return LH_METHOD_LH3;
    if (strncmp(sig, LH_SIG_LH4, LH_SIG_LEN) == 0) return LH_METHOD_LH4;
    if (strncmp(sig, LH_SIG_LH5, LH_SIG_LEN) == 0) return LH_METHOD_LH5;
    if (strncmp(sig, LH_SIG_LH6, LH_SIG_LEN) == 0) return LH_METHOD_LH6;
    if (strncmp(sig, LH_SIG_LH7, LH_SIG_LEN) == 0) return LH_METHOD_LH7;
    if (strncmp(sig, LH_SIG_LHX, LH_SIG_LEN) == 0) return LH_METHOD_LHX;
    if (strncmp(sig, LH_SIG_LZS, LH_SIG_LEN) == 0) return LH_METHOD_LZS;
    if (strncmp(sig, LH_SIG_LZ5, LH_SIG_LEN) == 0) return LH_METHOD_LZ5;
    if (strncmp(sig, LH_SIG_LZ4, LH_SIG_LEN) == 0) return LH_METHOD_LZ4;
    if (strncmp(sig, LH_SIG_LHD, LH_SIG_LEN) == 0) return LH_METHOD_LHD;
    if (strncmp(sig, LH_SIG_PM0, LH_SIG_LEN) == 0) return LH_METHOD_PM0;
    if (strncmp(sig, LH_SIG_PM1, LH_SIG_LEN) == 0) return LH_METHOD_PM1;
    if (strncmp(sig, LH_SIG_PM2, LH_SIG_LEN) == 0) return LH_METHOD_PM2;
    if (strncmp(sig, LH_SIG_PMS, LH_SIG_LEN) == 0) return LH_METHOD_PMS;
    if (strncmp(sig, LH_SIG_PC1, LH_SIG_LEN) == 0) return LH_METHOD_PC1;
    /*
     * Unknown method strings must not fall through to LH5: that fed
     * PMarc -pm1-/-pm2- bitstreams into the LH5 decoder and crashed.
     * PMS is already stubbed as unsupported in the codec switch.
     */
    return LH_METHOD_PMS;
}

const char *lh_method_to_string(lh_method method)
{
    switch (method) {
    case LH_METHOD_LH0: return LH_SIG_LH0;
    case LH_METHOD_LH1: return LH_SIG_LH1;
    case LH_METHOD_LH2: return LH_SIG_LH2;
    case LH_METHOD_LH3: return LH_SIG_LH3;
    case LH_METHOD_LH4: return LH_SIG_LH4;
    case LH_METHOD_LH5: return LH_SIG_LH5;
    case LH_METHOD_LH6: return LH_SIG_LH6;
    case LH_METHOD_LH7: return LH_SIG_LH7;
    case LH_METHOD_LHX: return LH_SIG_LHX;
    case LH_METHOD_LZS: return LH_SIG_LZS;
    case LH_METHOD_LZ5: return LH_SIG_LZ5;
    case LH_METHOD_LZ4: return LH_SIG_LZ4;
    case LH_METHOD_LHD: return LH_SIG_LHD;
    case LH_METHOD_PM0: return LH_SIG_PM0;
    case LH_METHOD_PM1: return LH_SIG_PM1;
    case LH_METHOD_PM2: return LH_SIG_PM2;
    case LH_METHOD_PMS: return LH_SIG_PMS;
    case LH_METHOD_PC1: return LH_SIG_PC1;
    default: return LH_SIG_LH5;
    }
}

lh_level lh_level_default(void)
{
    return LH_LEVEL_LH5;
}

lh_status lh_compress(
    lh_method method,
    const unsigned char *plain,
    size_t plain_len,
    unsigned char *out,
    size_t *out_len
)
{
    long rc;
    unsigned long sz;

    if (!plain || !out || !out_len || plain_len == 0) {
        return LH_ERR_INVALID_ARG;
    }
    sz = (unsigned long)*out_len;
    rc = lh_codec_compress((void *)plain, (unsigned long)plain_len,
        out, &sz, method);
    if (rc != 0) {
        if (rc == (long)LH_ERR_NO_MEMORY) {
            return LH_ERR_NO_MEMORY;
        }
        if (rc == (long)LH_ERR_UNSUPPORTED) {
            return LH_ERR_UNSUPPORTED;
        }
        return LH_ERR_INVALID_ARG;
    }
    *out_len = (size_t)sz;
    return LH_OK;
}

lh_status lh_decompress(
    lh_method method,
    const unsigned char *compressed,
    size_t compressed_len,
    size_t expected_len,
    unsigned char *out,
    size_t out_cap
)
{
    long rc;

    if (!compressed || !out || compressed_len == 0 || out_cap < expected_len) {
        return LH_ERR_INVALID_ARG;
    }
    rc = lh_codec_decompress((void *)compressed, (unsigned long)compressed_len,
        out, (unsigned long)expected_len, method);
    if (rc != 0) {
        return LH_ERR_INVALID_ARG;
    }
    return LH_OK;
}