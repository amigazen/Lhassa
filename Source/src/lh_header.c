/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_header.c - LHA header read/write (levels 0-2).
 *
 * Level 0/1 header read follows lhex/header.c (split buffer + extension chain).
 */

#include "lh_internal.h"

#include <stdio.h>
#include <string.h>

#define LH_HDR_BUF      1024
#define LH_I_CHECKSUM   1
#define LH_I_METHOD     2
#define LH_I_PACKED     7
#define LH_I_ORIGINAL   11
#define LH_I_STAMP      15
#define LH_I_ATTR       19
#define LH_I_LEVEL      20
#define LH_I_NLEN       21
#define LH_I_NAME       22

int lh_debug_verbose = 0;

void lh_set_debug_verbose(int on)
{
    lh_debug_verbose = on ? 1 : 0;
}

lh_u32 lh_read_le32(const unsigned char *p)
{
    return (lh_u32)(
        ((unsigned int)p[0])
      | ((unsigned int)p[1] << 8)
      | ((unsigned int)p[2] << 16)
      | ((unsigned int)p[3] << 24)
    );
}

void lh_write_le32(unsigned char *p, lh_u32 v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

lh_u16 lh_read_le16(const unsigned char *p)
{
    return (lh_u16)(((unsigned int)p[0]) | ((unsigned int)p[1] << 8));
}

void lh_write_le16(unsigned char *p, lh_u16 v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

void lh_hdr_meta_clear(lh_hdr_meta *m)
{
    if (!m) {
        return;
    }
    free(m->filename);
    free(m->comment);
    memset(m, 0, sizeof(*m));
}

static unsigned char lh_hdr_checksum(const unsigned char *data, size_t len)
{
    unsigned char sum = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        sum = (unsigned char)(sum + data[i]);
    }
    return sum;
}

static void lh_hdr_debug(const char *tag, FILE *fp, const lh_hdr_meta *meta, lh_status st)
{
    long pos;

    if (!lh_debug_verbose) {
        return;
    }
    pos = ftell(fp);
    fprintf(stderr,
        "lhassa: hdr %s st=%s pos=%ld lvl=%u %.5s packed=%lu orig=%lu name=%s\n",
        tag,
        lh_status_string(st),
        pos,
        (unsigned int)meta->header_level,
        meta->method_sig,
        meta->packed_size,
        meta->original_size,
        meta->filename ? meta->filename : "(null)");
}

/* lhex-style buffer walk (data[0] unused; fields start at data[1]). */
static unsigned char *lh_get_ptr;

static void lh_setup_get(unsigned char *p)
{
    lh_get_ptr = p;
}

static unsigned int lh_get_byte(void)
{
    return (unsigned int)(*lh_get_ptr++ & 0xff);
}

static unsigned short lh_get_word(void)
{
    unsigned int b0;
    unsigned int b1;

    b0 = lh_get_byte();
    b1 = lh_get_byte();
    return (unsigned short)((b1 << 8) + b0);
}

static unsigned long lh_get_longword(void)
{
    unsigned long b0;
    unsigned long b1;
    unsigned long b2;
    unsigned long b3;

    b0 = (unsigned long)lh_get_byte();
    b1 = (unsigned long)lh_get_byte();
    b2 = (unsigned long)lh_get_byte();
    b3 = (unsigned long)lh_get_byte();
    return (b3 << 24) + (b2 << 16) + (b1 << 8) + b0;
}

static void lh_hdr_read_base_from_buf(lh_hdr_meta *meta, unsigned char *buf)
{
    unsigned char name_len;

    memcpy(meta->method_sig, buf + 2, LH_SIG_LEN);
    meta->method_sig[LH_SIG_LEN] = '\0';
    meta->method = lh_method_from_string((char *)meta->method_sig);
    meta->packed_size = lh_read_le32(buf + 7);
    meta->original_size = lh_read_le32(buf + 11);
    meta->timestamp = lh_read_le32(buf + 15);
    meta->attribute = buf[19];
    meta->header_level = buf[20];
    name_len = buf[21];
    if (name_len > 0) {
        meta->filename = (char *)malloc((size_t)name_len + 1u);
        if (meta->filename) {
            memcpy(meta->filename, buf + 22, name_len);
            meta->filename[name_len] = '\0';
        }
    }
    meta->is_directory = (meta->method == LH_METHOD_LHD);
}

static lh_status lh_hdr_skip_bytes(FILE *fp, size_t n)
{
    unsigned char scratch[256];
    size_t chunk;

    while (n > 0) {
        chunk = n;
        if (chunk > sizeof(scratch)) {
            chunk = sizeof(scratch);
        }
        if (fread(scratch, 1, chunk, fp) != chunk) {
            return LH_ERR_TRUNCATED;
        }
        n -= chunk;
    }
    return LH_OK;
}

static int lh_mf_read(
    FILE *fp,
    const unsigned char *mem,
    size_t mem_len,
    size_t *mem_pos,
    void *out,
    size_t n)
{
    unsigned char *dst;
    size_t avail;
    size_t take;

    dst = (unsigned char *)out;
    while (n > 0) {
        avail = 0;
        if (*mem_pos < mem_len) {
            avail = mem_len - *mem_pos;
        }
        take = n;
        if (take > avail) {
            take = avail;
        }
        if (take > 0) {
            memcpy(dst, mem + *mem_pos, take);
            *mem_pos += take;
            dst += take;
            n -= take;
        }
        if (n > 0) {
            if (fread(dst, 1, n, fp) != n) {
                return 0;
            }
            dst += n;
            n = 0;
        }
    }
    return 1;
}

/* Level-2 extensions live entirely in the header buffer (lha-114i: no fread). */
static lh_status lh_hdr_read_extensions_buf(
    lh_hdr_meta *meta,
    const unsigned char *mem,
    size_t mem_len,
    unsigned long *ext_total)
{
    size_t pos;
    unsigned short blk_len;
    unsigned char blk_type;
    size_t payload;
    char dirbuf[512];
    size_t dir_len;
    char *combined;
    size_t dlen;
    size_t flen;
    unsigned long consumed;

    dirbuf[0] = '\0';
    dir_len = 0;
    pos = 0;
    consumed = 0;
    if (ext_total) {
        *ext_total = 0;
    }

    while (pos + 2u <= mem_len) {
        blk_len = lh_read_le16(mem + pos);
        pos += 2;
        consumed += 2;
        if (blk_len == 0) {
            break;
        }
        if (blk_len < 3 || pos + (size_t)blk_len - 3u > mem_len) {
            return LH_ERR_BAD_HEADER;
        }
        blk_type = mem[pos++];
        consumed += 1;
        payload = (size_t)blk_len - 3u;

        if (blk_type == 1) {
            free(meta->filename);
            meta->filename = NULL;
            if (payload > 0) {
                meta->filename = (char *)malloc(payload + 1u);
                if (!meta->filename) {
                    return LH_ERR_NO_MEMORY;
                }
                memcpy(meta->filename, mem + pos, payload);
                meta->filename[payload] = '\0';
            }
        } else if (blk_type == 2) {
            if (payload > 0 && payload < sizeof(dirbuf)) {
                memcpy(dirbuf, mem + pos, payload);
                dirbuf[payload] = '\0';
                dir_len = payload;
            }
        }
        pos += payload;
        consumed += payload;
    }

    if (dir_len > 0 && meta->filename && meta->filename[0]) {
        dlen = dir_len;
        flen = strlen(meta->filename);
        combined = (char *)malloc(dlen + flen + 2u);
        if (!combined) {
            return LH_ERR_NO_MEMORY;
        }
        memcpy(combined, dirbuf, dlen);
        if (dlen > 0 && dirbuf[dlen - 1] != '/' && dirbuf[dlen - 1] != ':') {
            combined[dlen] = '/';
            dlen++;
        }
        memcpy(combined + dlen, meta->filename, flen + 1u);
        free(meta->filename);
        meta->filename = combined;
    }

    if (ext_total) {
        *ext_total = consumed;
    }
    return LH_OK;
}

static int lh_mf_skip(
    FILE *fp,
    const unsigned char *mem,
    size_t mem_len,
    size_t *mem_pos,
    size_t n)
{
    unsigned char scratch[256];
    size_t chunk;

    while (n > 0) {
        chunk = n;
        if (chunk > sizeof(scratch)) {
            chunk = sizeof(scratch);
        }
        if (!lh_mf_read(fp, mem, mem_len, mem_pos, scratch, chunk)) {
            return 0;
        }
        n -= chunk;
    }
    return 1;
}

static lh_status lh_hdr_read_extensions(
    FILE *fp,
    lh_hdr_meta *meta,
    const unsigned char *mem,
    size_t mem_len,
    unsigned long *ext_total)
{
    unsigned short blk_len;
    unsigned char blk_type;
    size_t payload;
    size_t mem_pos;
    char dirbuf[512];
    size_t dir_len;
    char *combined;
    size_t dlen;
    size_t flen;
    unsigned long consumed;
    lh_status st;

    dirbuf[0] = '\0';
    dir_len = 0;
    consumed = 0;
    mem_pos = 0;
    if (ext_total) {
        *ext_total = 0;
    }

    for (;;) {
        if (!lh_mf_read(fp, mem, mem_len, &mem_pos, &blk_len, 2)) {
            return LH_ERR_TRUNCATED;
        }
        consumed += 2;
        if (blk_len == 0) {
            break;
        }
        if (blk_len < 3) {
            return LH_ERR_BAD_HEADER;
        }
        payload = (size_t)blk_len - 3u;
        if (!lh_mf_read(fp, mem, mem_len, &mem_pos, &blk_type, 1)) {
            return LH_ERR_TRUNCATED;
        }
        consumed += 1;

        if (blk_type == 1) {
            free(meta->filename);
            meta->filename = NULL;
            if (payload > 0) {
                meta->filename = (char *)malloc(payload + 1u);
                if (!meta->filename) {
                    return LH_ERR_NO_MEMORY;
                }
                if (!lh_mf_read(fp, mem, mem_len, &mem_pos, meta->filename, payload)) {
                    free(meta->filename);
                    meta->filename = NULL;
                    return LH_ERR_TRUNCATED;
                }
                meta->filename[payload] = '\0';
            }
        } else if (blk_type == 2) {
            if (payload >= sizeof(dirbuf)) {
                if (!lh_mf_skip(fp, mem, mem_len, &mem_pos, payload)) {
                    return LH_ERR_TRUNCATED;
                }
            } else if (payload > 0) {
                if (!lh_mf_read(fp, mem, mem_len, &mem_pos, dirbuf, payload)) {
                    return LH_ERR_TRUNCATED;
                }
                dirbuf[payload] = '\0';
                dir_len = payload;
            }
        } else if (payload > 0) {
            unsigned char scratch[256];
            size_t left;
            size_t chunk;

            left = payload;
            while (left > 0) {
                chunk = left;
                if (chunk > sizeof(scratch)) {
                    chunk = sizeof(scratch);
                }
                if (!lh_mf_read(fp, mem, mem_len, &mem_pos, scratch, chunk)) {
                    return LH_ERR_TRUNCATED;
                }
                left -= chunk;
            }
        }
        consumed += payload;
    }

    if (dir_len > 0 && meta->filename && meta->filename[0]) {
        dlen = dir_len;
        flen = strlen(meta->filename);
        combined = (char *)malloc(dlen + flen + 2u);
        if (!combined) {
            return LH_ERR_NO_MEMORY;
        }
        memcpy(combined, dirbuf, dlen);
        if (dlen > 0 && dirbuf[dlen - 1] != '/' && dirbuf[dlen - 1] != ':') {
            combined[dlen] = '/';
            dlen++;
        }
        memcpy(combined + dlen, meta->filename, flen + 1u);
        free(meta->filename);
        meta->filename = combined;
    }

    if (ext_total) {
        *ext_total = consumed;
    }
    return LH_OK;
}

static lh_status lh_hdr_read_level2(FILE *fp, lh_hdr_meta *meta, unsigned char *buf)
{
    int header_size;
    unsigned long ext_total;
    lh_status st;
    int padding;
    unsigned char pb;
    size_t mem_len;

    header_size = (int)buf[0] + (int)((unsigned int)buf[1] << 8);
    if (header_size < 26) {
        return LH_ERR_BAD_HEADER;
    }

    memcpy(meta->method_sig, buf + LH_I_METHOD, LH_SIG_LEN);
    meta->method_sig[LH_SIG_LEN] = '\0';
    meta->method = lh_method_from_string(meta->method_sig);
    meta->is_directory = (meta->method == LH_METHOD_LHD);

    meta->packed_size = lh_read_le32(buf + LH_I_PACKED);
    meta->original_size = lh_read_le32(buf + LH_I_ORIGINAL);
    meta->timestamp = lh_read_le32(buf + LH_I_STAMP);
    meta->attribute = buf[LH_I_ATTR];
    meta->header_level = 2;
    meta->has_crc = 1;
    meta->crc = lh_read_le16(buf + 21);
    meta->os_id = buf[23];

    /* Level-2 extension chain is wholly inside the header buffer. */
    mem_len = 0;
    if ((size_t)header_size > 24u) {
        mem_len = (size_t)header_size - 24u;
    }
    ext_total = 0;
    st = lh_hdr_read_extensions_buf(meta, buf + 24, mem_len, &ext_total);
    if (st != LH_OK) {
        return st;
    }

    /* Bytes 24-25 are in the 26-byte base; body starts at offset 26. */
    if (ext_total >= 2u) {
        ext_total -= 2u;
    }

    padding = header_size - 26 - (int)ext_total;
    if (padding < 0 || padding > 1) {
        return LH_ERR_BAD_HEADER;
    }
    while (padding > 0) {
        if (fread(&pb, 1, 1, fp) != 1) {
            return LH_ERR_TRUNCATED;
        }
        padding--;
    }
    if (!meta->filename) {
        return LH_ERR_BAD_HEADER;
    }
    return LH_OK;
}

static lh_status lh_hdr_read_lhex(FILE *fp, lh_hdr_meta *meta, unsigned char *data)
{
    int header_size;
    int name_length;
    int checksum;
    int header_level;
    int extend_type;
    int i;

    header_size = (int)data[0];
    name_length = 0;
    extend_type = 0;

    lh_setup_get(data + LH_I_CHECKSUM);
    checksum = (int)lh_get_byte();

    memcpy(meta->method_sig, data + LH_I_METHOD, LH_SIG_LEN);
    meta->method_sig[LH_SIG_LEN] = '\0';
    meta->method = lh_method_from_string(meta->method_sig);
    meta->is_directory = (meta->method == LH_METHOD_LHD);

    lh_setup_get(data + LH_I_PACKED);
    meta->packed_size = (unsigned long)lh_get_longword();
    meta->original_size = (unsigned long)lh_get_longword();
    meta->timestamp = (unsigned long)lh_get_longword();
    meta->attribute = (unsigned char)lh_get_byte();

    header_level = (int)lh_get_byte();
    meta->header_level = (unsigned char)header_level;

    if (header_level != 2) {
        if (lh_hdr_checksum(data + LH_I_METHOD, (size_t)header_size) != (unsigned char)checksum) {
            /* LhA warns and continues; do not fail the archive. */
        }
        name_length = (int)lh_get_byte();
        if (name_length > 0) {
            meta->filename = (char *)malloc((size_t)name_length + 1u);
            if (!meta->filename) {
                return LH_ERR_NO_MEMORY;
            }
            for (i = 0; i < name_length; i++) {
                meta->filename[i] = (char)lh_get_byte();
            }
            meta->filename[name_length] = '\0';
        }
    }

    if (header_size - name_length >= 24) {
        meta->has_crc = 1;
        meta->crc = lh_get_word();
        extend_type = (int)lh_get_byte();
        meta->os_id = (unsigned char)extend_type;
    } else if (header_size - name_length == 22) {
        meta->has_crc = 1;
        meta->crc = lh_get_word();
        extend_type = 0;
        meta->os_id = 0;
    } else if (header_size - name_length == 20) {
        meta->has_crc = 0;
        extend_type = 0;
        meta->os_id = 0;
    } else {
        return LH_ERR_BAD_HEADER;
    }

    if (extend_type == (int)'U' && header_level == 0) {
        (void)lh_get_byte();
        (void)lh_get_longword();
        (void)lh_get_word();
        (void)lh_get_word();
        (void)lh_get_word();
        return LH_OK;
    }

    if (header_level == 1) {
        int extend_size;

        extend_size = header_size - name_length - 25;
        while (extend_size-- > 0) {
            (void)lh_get_byte();
        }
    }

    if (header_level > 0) {
        unsigned char *ext_ptr;
        unsigned char *ext_lim;
        int blk_len;
        char dirbuf[512];
        size_t dir_len;
        char *combined;
        size_t dlen;
        size_t flen;

        dirbuf[0] = '\0';
        dir_len = 0;
        if (header_level != 2) {
            lh_setup_get(data + header_size);
            ext_ptr = lh_get_ptr;
            ext_lim = data + LH_HDR_BUF;
            while ((blk_len = (int)lh_get_word()) != 0) {
                if ((size_t)(ext_lim - lh_get_ptr) < (size_t)blk_len) {
                    return LH_ERR_BAD_HEADER;
                }
                if (fread(lh_get_ptr, 1, (size_t)blk_len, fp) != (size_t)blk_len) {
                    return LH_ERR_TRUNCATED;
                }
                switch (lh_get_byte()) {
                case 1:
                    free(meta->filename);
                    meta->filename = NULL;
                    if (blk_len > 3) {
                        meta->filename = (char *)malloc((size_t)blk_len - 2u);
                        if (!meta->filename) {
                            return LH_ERR_NO_MEMORY;
                        }
                        for (i = 0; i < blk_len - 3; i++) {
                            meta->filename[i] = (char)lh_get_byte();
                        }
                        meta->filename[blk_len - 3] = '\0';
                    }
                    break;
                case 2:
                    if (blk_len > 3 && (size_t)blk_len - 3u < sizeof(dirbuf)) {
                        for (i = 0; i < blk_len - 3; i++) {
                            dirbuf[i] = (char)lh_get_byte();
                        }
                        dirbuf[blk_len - 3] = '\0';
                        dir_len = (size_t)(blk_len - 3);
                    } else {
                        lh_setup_get(lh_get_ptr + (size_t)blk_len - 3u);
                    }
                    break;
                case 0x50:
                    if (extend_type == (int)'U') {
                        (void)lh_get_word();
                    } else {
                        lh_setup_get(lh_get_ptr + (size_t)blk_len - 3u);
                    }
                    break;
                case 0x51:
                    if (extend_type == (int)'U') {
                        (void)lh_get_word();
                        (void)lh_get_word();
                    } else {
                        lh_setup_get(lh_get_ptr + (size_t)blk_len - 3u);
                    }
                    break;
                case 0x54:
                    if (extend_type == (int)'U') {
                        (void)lh_get_longword();
                    } else {
                        lh_setup_get(lh_get_ptr + (size_t)blk_len - 3u);
                    }
                    break;
                default:
                    lh_setup_get(lh_get_ptr + (size_t)blk_len - 3u);
                    break;
                }
            }
            if (lh_get_ptr - ext_ptr != 2) {
                meta->packed_size -= (unsigned long)(lh_get_ptr - ext_ptr - 2);
            }
        }
        if (dir_len > 0 && meta->filename && meta->filename[0]) {
            dlen = dir_len;
            flen = strlen(meta->filename);
            combined = (char *)malloc(dlen + flen + 2u);
            if (!combined) {
                return LH_ERR_NO_MEMORY;
            }
            memcpy(combined, dirbuf, dlen);
            if (dlen > 0 && dirbuf[dlen - 1] != '/' && dirbuf[dlen - 1] != ':') {
                combined[dlen] = '/';
                dlen++;
            }
            memcpy(combined + dlen, meta->filename, flen + 1u);
            free(meta->filename);
            meta->filename = combined;
        }
    }

    return LH_OK;
}

lh_status lh_hdr_read(FILE *fp, lh_hdr_meta *meta, unsigned char write_level)
{
    unsigned char buf[LH_HDR_BUF];
    unsigned char hdr_len;
    int header_size;
    int header_level;
    size_t n;
    lh_status st;

    (void)write_level;

    if (!fp || !meta) {
        return LH_ERR_INVALID_ARG;
    }
    lh_hdr_meta_clear(meta);
    memset(buf, 0, sizeof(buf));

    n = fread(&hdr_len, 1, 1, fp);
    if (n != 1) {
        return LH_ERR_TRUNCATED;
    }
    if (hdr_len == 0) {
        return LH_ERR_TRUNCATED;
    }
    if (hdr_len < 20) {
        return LH_ERR_BAD_HEADER;
    }
    if ((size_t)hdr_len >= LH_HDR_BUF - 4u) {
        return LH_ERR_BAD_HEADER;
    }

    buf[0] = hdr_len;
    header_size = (int)hdr_len;

    /*
     * lhex/header.c: fread header_size-1, then 2-byte prefetch for level != 2.
     * The prefetch byte(s) sit between the counted header body and packed data.
     */
    n = fread(buf + LH_I_CHECKSUM, 1, (size_t)header_size - 1u, fp);
    if (n != (size_t)header_size - 1u) {
        return LH_ERR_TRUNCATED;
    }
    header_level = (int)buf[LH_I_LEVEL];

    if (header_level == 2) {
        int initial_size;

        initial_size = header_size;
        header_size = (int)buf[0] + (int)((unsigned int)buf[1] << 8);
        if (header_size < 26 || header_size >= LH_HDR_BUF - 4) {
            return LH_ERR_BAD_HEADER;
        }
        if (header_size > initial_size) {
            n = fread(buf + initial_size, 1, (size_t)header_size - (size_t)initial_size, fp);
            if (n != (size_t)header_size - (size_t)initial_size) {
                return LH_ERR_TRUNCATED;
            }
        }
    } else if (fread(buf + header_size, 1, 2, fp) != 2) {
        return LH_ERR_TRUNCATED;
    }

    if (buf[LH_I_METHOD] != '-' || buf[LH_I_METHOD + 1] != 'l' || buf[LH_I_METHOD + 2] != 'h') {
        return LH_ERR_BAD_HEADER;
    }

    if (header_level == 2) {
        st = lh_hdr_read_level2(fp, meta, buf);
        lh_hdr_debug("read", fp, meta, st);
        return st;
    }

    st = lh_hdr_read_lhex(fp, meta, buf);
    lh_hdr_debug("read", fp, meta, st);
    return st;
}

lh_status lh_hdr_write(FILE *fp, const lh_hdr_meta *meta, unsigned char write_level)
{
    unsigned char buf[260];
    size_t name_len;
    size_t hdr_body;
    unsigned char checksum;
    size_t i;

    if (!fp || !meta || !meta->filename) {
        return LH_ERR_INVALID_ARG;
    }
    name_len = strlen(meta->filename);
    if (name_len > 200) {
        return LH_ERR_FILENAME_TOO_LONG;
    }
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 2, meta->method_sig, LH_SIG_LEN);
    lh_write_le32(buf + 7, meta->packed_size);
    lh_write_le32(buf + 11, meta->original_size);
    lh_write_le32(buf + 15, meta->timestamp);
    buf[19] = meta->attribute;
    buf[20] = write_level;
    buf[21] = (unsigned char)name_len;
    memcpy(buf + 22, meta->filename, name_len);
    lh_write_le16(buf + 22 + name_len, meta->crc);
    buf[22 + name_len + 2] = meta->os_id ? meta->os_id : LH_OS_AMIGA;
    if (write_level >= 1) {
        lh_write_le16(buf + 22 + name_len + 3, 0);
        hdr_body = 22 + name_len + 5;
    } else {
        hdr_body = 22 + name_len + 2;
    }
    if (hdr_body > 255) {
        return LH_ERR_BAD_HEADER;
    }
    buf[0] = (unsigned char)hdr_body;
    checksum = 0;
    for (i = 2; i < hdr_body; i++) {
        checksum = (unsigned char)(checksum + buf[i]);
    }
    buf[1] = checksum;
    /* lhex wire format: size, body[0..size-2], then 2 extension/prefetch bytes. */
    if (fwrite(buf, 1, 1, fp) != 1) {
        return LH_ERR_IO;
    }
    if (fwrite(buf + 1, 1, hdr_body - 1, fp) != hdr_body - 1) {
        return LH_ERR_IO;
    }
    if (fwrite(buf + hdr_body, 1, 2, fp) != 2) {
        return LH_ERR_IO;
    }
    if (write_level >= 2) {
        /* Terminate extension chain (level-2 readers expect this word). */
        if (fwrite("\0\0", 1, 2, fp) != 2) {
            return LH_ERR_IO;
        }
    }
    return LH_OK;
}
