/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_archive.c - LHA archive container read/write.
 */

#include "lh_internal.h"

#ifdef HOST
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef HOST
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/dos.h>

#ifndef TICKS_PER_SECOND
#define LH_TPS 50
#else
#define LH_TPS TICKS_PER_SECOND
#endif

/* AmigaDOS DateStamp epoch is 1978-01-01; offset from Unix epoch in seconds. */
#define LH_AMIGA_EPOCH_SEC ((8UL * 365UL + 2UL) * 86400UL)

/* Lock+Examine: vbcc POSIX stat/fstat do not resolve ///assign paths. */
static int lh_capture_mtime_amiga(const char *path, lh_datetime *dt)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    unsigned long secs;
    int ok;

    ok = 0;
    if (!path || !dt) {
        return 0;
    }
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) {
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (long)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        return 0;
    }
    if (Examine(lock, fib)) {
        secs = (unsigned long)fib->fib_Date.ds_Days * 86400UL;
        secs += (unsigned long)fib->fib_Date.ds_Minute * 60UL;
        secs += (unsigned long)fib->fib_Date.ds_Tick / (unsigned long)LH_TPS;
        secs += LH_AMIGA_EPOCH_SEC;
        lh_datetime_from_time_t(dt, (long)secs);
        ok = 1;
    }
    FreeMem((APTR)fib, (long)sizeof(struct FileInfoBlock));
    UnLock(lock);
    return ok;
}
#endif

struct lh_writer {
    FILE *fp;
    unsigned char header_level;
    lh_level default_level;
};

struct lh_reader {
    FILE *fp;
    char password[256];
    int has_password;
    int header_only;
    lh_decrypt_state decrypt;
    lh_file_begin_fn on_begin;
    lh_progress_fn on_progress;
    lh_file_end_fn on_end;
    void *progress_ctx;
    int eof;
    lh_datetime archive_dt;
    int has_archive_dt;
};

/* Capture archive file stamp for list totals (see lh_reader_archive_datetime). */
static void lh_reader_capture_mtime(lh_reader *r, const char *path)
{
    if (!r) {
        return;
    }
    r->has_archive_dt = 0;
#ifndef HOST
    if (path && lh_capture_mtime_amiga(path, &r->archive_dt)) {
        r->has_archive_dt = 1;
    }
#else
    {
        struct stat st;

        if (r->fp && fstat(fileno(r->fp), &st) == 0) {
            lh_datetime_from_time_t(&r->archive_dt, (long)st.st_mtime);
            r->has_archive_dt = 1;
            return;
        }
        if (path && stat(path, &st) == 0) {
            lh_datetime_from_time_t(&r->archive_dt, (long)st.st_mtime);
            r->has_archive_dt = 1;
        }
    }
#endif
}

static lh_method lh_level_to_method(lh_level level, int store_only)
{
    if (store_only) {
        return LH_METHOD_LH0;
    }
    switch (level) {
    case LH_LEVEL_STORE: return LH_METHOD_LH0;
    case LH_LEVEL_LH1: return LH_METHOD_LH1;
    case LH_LEVEL_LH6: return LH_METHOD_LH6;
    case LH_LEVEL_LH7: return LH_METHOD_LH7;
    case LH_LEVEL_LH5:
    default: return LH_METHOD_LH5;
    }
}

static void lh_meta_to_entry(const lh_hdr_meta *meta, lh_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    if (meta->filename) {
        size_t flen = strlen(meta->filename);
        entry->filename = (char *)malloc(flen + 1);
        if (entry->filename) {
            memcpy(entry->filename, meta->filename, flen + 1);
        }
    }
    if (meta->comment) {
        size_t clen = strlen(meta->comment);
        entry->comment = (char *)malloc(clen + 1);
        if (entry->comment) {
            memcpy(entry->comment, meta->comment, clen + 1);
        }
    }
    entry->attrs = meta->attribute;
    if (meta->header_level >= 2) {
        lh_datetime_from_time_t(&entry->datetime, (long)meta->timestamp);
    } else {
        lh_datetime_unpack(meta->timestamp, &entry->datetime);
    }
    entry->data_len = meta->original_size;
    entry->packed_len = meta->packed_size;
    entry->method = meta->method;
    entry->os_id = meta->os_id;
    entry->header_level = meta->header_level;
    entry->crc = meta->crc;
    entry->is_directory = meta->is_directory;
}

void lh_entry_clear(lh_entry *entry)
{
    if (!entry) {
        return;
    }
    free(entry->filename);
    free(entry->comment);
    free(entry->data);
    memset(entry, 0, sizeof(*entry));
}

lh_writer *lh_writer_open(const char *path, unsigned char header_level,
    lh_level def_level, lh_status *err)
{
    lh_writer *w;

    if (!path) {
        if (err) {
            *err = LH_ERR_INVALID_ARG;
        }
        return NULL;
    }
    w = (lh_writer *)calloc(1, sizeof(*w));
    if (!w) {
        if (err) {
            *err = LH_ERR_NO_MEMORY;
        }
        return NULL;
    }
    w->fp = fopen(path, "wb");
    if (!w->fp) {
        free(w);
        if (err) {
            *err = LH_ERR_IO;
        }
        return NULL;
    }
    w->header_level = header_level;
    w->default_level = def_level;
    if (err) {
        *err = LH_OK;
    }
    return w;
}

lh_writer *lh_writer_open_append(const char *path, lh_status *err)
{
    lh_writer *w;
    FILE *test;

    test = fopen(path, "rb");
    if (test) {
        fclose(test);
        w = (lh_writer *)calloc(1, sizeof(*w));
        if (!w) {
            if (err) {
                *err = LH_ERR_NO_MEMORY;
            }
            return NULL;
        }
        w->fp = fopen(path, "ab");
        if (!w->fp) {
            free(w);
            if (err) {
                *err = LH_ERR_IO;
            }
            return NULL;
        }
        w->header_level = 2;
        w->default_level = LH_LEVEL_LH5;
        if (err) {
            *err = LH_OK;
        }
        return w;
    }
    return lh_writer_open(path, 2, LH_LEVEL_LH5, err);
}

lh_status lh_writer_add(
    lh_writer *w,
    const char *filename,
    const char *comment,
    lh_attrs attrs,
    const lh_datetime *datetime,
    lh_level level,
    int store_only,
    const unsigned char *data,
    size_t data_len
)
{
    lh_hdr_meta meta;
    unsigned char *packed = NULL;
    size_t packed_cap;
    size_t packed_len;
    lh_method method;
    lh_status st;
    unsigned long ts;
    long rc;

    (void)comment;

    if (!w || !w->fp || !filename) {
        return LH_ERR_INVALID_ARG;
    }
    memset(&meta, 0, sizeof(meta));
    method = lh_level_to_method(level, store_only);
    strncpy(meta.method_sig, lh_method_to_string(method), LH_SIG_LEN);
    meta.method = method;
    meta.original_size = (unsigned long)data_len;
    meta.attribute = attrs;
    meta.os_id = LH_OS_AMIGA;
    meta.header_level = w->header_level;
    meta.filename = (char *)filename;
    if (datetime) {
        st = lh_datetime_pack(datetime, &ts);
        if (st != LH_OK) {
            return st;
        }
        meta.timestamp = ts;
    } else {
        lh_datetime now;
        lh_datetime_from_time_t(&now, (long)time(NULL));
        meta.timestamp = lh_dos_timestamp_pack(&now);
    }
    if (data_len == 0 || method == LH_METHOD_LHD) {
        meta.packed_size = 0;
        meta.is_directory = 1;
        strncpy(meta.method_sig, LH_SIG_LHD, LH_SIG_LEN);
        meta.method = LH_METHOD_LHD;
    } else {
        packed_cap = data_len + data_len / 8 + 512;
        packed = (unsigned char *)malloc(packed_cap);
        if (!packed) {
            return LH_ERR_NO_MEMORY;
        }
        packed_len = packed_cap;
        rc = lh_codec_compress((void *)data, (unsigned long)data_len,
            packed, (unsigned long *)&packed_len, (long)method);
        if (rc != 0) {
            free(packed);
            return LH_ERR_INVALID_ARG;
        }
        meta.packed_size = (unsigned long)packed_len;
        meta.crc = lh_crc16(data, data_len);
        meta.has_crc = 1;
        st = lh_hdr_write(w->fp, &meta, w->header_level);
        if (st != LH_OK) {
            free(packed);
            return st;
        }
        if (packed_len > 0 && fwrite(packed, 1, packed_len, w->fp) != packed_len) {
            free(packed);
            return LH_ERR_IO;
        }
        free(packed);
        return LH_OK;
    }
    meta.crc = 0;
    st = lh_hdr_write(w->fp, &meta, w->header_level);
    return st;
}

lh_status lh_writer_close(lh_writer **w)
{
    if (!w || !*w) {
        return LH_ERR_INVALID_ARG;
    }
    if ((*w)->fp) {
        fclose((*w)->fp);
    }
    free(*w);
    *w = NULL;
    return LH_OK;
}

lh_reader *lh_reader_open(const char *path, lh_status *err)
{
    lh_reader *r;

    if (!path) {
        if (err) {
            *err = LH_ERR_INVALID_ARG;
        }
        return NULL;
    }
    r = (lh_reader *)calloc(1, sizeof(*r));
    if (!r) {
        if (err) {
            *err = LH_ERR_NO_MEMORY;
        }
        return NULL;
    }
    r->fp = fopen(path, "rb");
    if (!r->fp) {
        free(r);
        if (err) {
            *err = LH_ERR_IO;
        }
        return NULL;
    }
    lh_reader_capture_mtime(r, path);
    if (err) {
        *err = LH_OK;
    }
    return r;
}

int lh_reader_archive_datetime(const lh_reader *r, lh_datetime *dt)
{
    if (!r || !dt) {
        return 0;
    }
    if (!r->has_archive_dt) {
        return 0;
    }
    *dt = r->archive_dt;
    return 1;
}

lh_status lh_reader_set_password(lh_reader *r, const char *password)
{
    if (!r) {
        return LH_ERR_INVALID_ARG;
    }
    if (!password || !password[0]) {
        r->has_password = 0;
        r->password[0] = '\0';
        return LH_OK;
    }
    strncpy(r->password, password, sizeof(r->password) - 1);
    r->password[sizeof(r->password) - 1] = '\0';
    r->has_password = 1;
    lh_decrypt_init(&r->decrypt, r->password);
    return LH_OK;
}

void lh_reader_set_progress(
    lh_reader *r,
    lh_file_begin_fn on_begin,
    lh_progress_fn on_progress,
    lh_file_end_fn on_end,
    void *ctx
)
{
    if (!r) {
        return;
    }
    r->on_begin = on_begin;
    r->on_progress = on_progress;
    r->on_end = on_end;
    r->progress_ctx = ctx;
}

void lh_reader_set_header_only(lh_reader *r, int on)
{
    if (!r) {
        return;
    }
    r->header_only = on ? 1 : 0;
}

lh_status lh_reader_next(lh_reader *r, lh_entry *entry)
{
    lh_hdr_meta meta;
    unsigned char *packed = NULL;
    unsigned char *plain = NULL;
    lh_status st;
    size_t n;
    unsigned short computed;
    long rc;

    if (!r || !entry) {
        return LH_ERR_INVALID_ARG;
    }
    if (r->eof) {
        entry->filename = NULL;
        return LH_OK;
    }
    memset(&meta, 0, sizeof(meta));
    st = lh_hdr_read(r->fp, &meta, 0);
    if (st == LH_ERR_TRUNCATED) {
        r->eof = 1;
        entry->filename = NULL;
        return LH_OK;
    }
    if (st != LH_OK) {
        return st;
    }
    lh_meta_to_entry(&meta, entry);
    if (r->on_begin && entry->filename) {
        r->on_begin(r->progress_ctx, entry->filename, entry->data_len);
    }
    if (meta.packed_size == 0 || meta.is_directory) {
        lh_hdr_meta_clear(&meta);
        return LH_OK;
    }
    if (r->header_only) {
        if (fseek(r->fp, (long)meta.packed_size, SEEK_CUR) != 0) {
            lh_hdr_meta_clear(&meta);
            lh_entry_clear(entry);
            return LH_ERR_IO;
        }
        entry->crc_ok = 1;
        lh_hdr_meta_clear(&meta);
        return LH_OK;
    }
    packed = (unsigned char *)malloc(meta.packed_size);
    if (!packed) {
        lh_hdr_meta_clear(&meta);
        lh_entry_clear(entry);
        return LH_ERR_NO_MEMORY;
    }
    n = fread(packed, 1, meta.packed_size, r->fp);
    if (n != meta.packed_size) {
        free(packed);
        lh_hdr_meta_clear(&meta);
        lh_entry_clear(entry);
        return LH_ERR_TRUNCATED;
    }
    if (r->has_password) {
        lh_decrypt_buffer(&r->decrypt, packed, meta.packed_size);
    }
    plain = (unsigned char *)malloc(meta.original_size ? meta.original_size : 1);
    if (!plain) {
        free(packed);
        lh_hdr_meta_clear(&meta);
        lh_entry_clear(entry);
        return LH_ERR_NO_MEMORY;
    }
    rc = lh_codec_decompress(packed, meta.packed_size, plain,
        meta.original_size, (long)meta.method);
    free(packed);
    if (rc != 0) {
        free(plain);
        lh_hdr_meta_clear(&meta);
        lh_entry_clear(entry);
        return LH_ERR_INVALID_ARCHIVE;
    }
    computed = lh_crc16(plain, meta.original_size);
    entry->data = plain;
    entry->data_len = meta.original_size;
    entry->crc_ok = (meta.has_crc == 0 || computed == meta.crc);
    if (r->on_progress) {
        r->on_progress(r->progress_ctx, meta.original_size, meta.original_size);
    }
    if (r->on_end && entry->filename) {
        r->on_end(r->progress_ctx, entry->filename, entry->crc_ok,
            meta.crc, computed, meta.original_size);
    }
    lh_hdr_meta_clear(&meta);
    return LH_OK;
}

void lh_reader_close(lh_reader **r)
{
    if (!r || !*r) {
        return;
    }
    if ((*r)->fp) {
        fclose((*r)->fp);
    }
    free(*r);
    *r = NULL;
}
