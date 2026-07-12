/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_archive.c - LHA archive container read/write.
 */

#include "lh_internal.h"

#if !defined(LH_AMIGA) && !defined(LH_HOST)
#error "lh_archive.c: set LH_AMIGA (library) or LH_HOST (make HOST=1)"
#endif

#ifdef LH_HOST
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef LH_AMIGA
#define __USE_SYSBASE
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* Lock+Examine for archive mtime. */
static int lh_capture_mtime_amiga(const char *path, lh_datetime *dt)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int ok;

    ok = 0;
    if (!path || !dt) {
        return 0;
    }
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) {
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        return 0;
    }
    if (Examine(lock, fib)) {
        ok = 1;
        lh_datetime_from_datestamp(&fib->fib_Date, dt);
    }
    FreeMem(fib, sizeof(struct FileInfoBlock));
    UnLock(lock);
    return ok;
}
#endif

void lh_stream_init(lh_stream *s)
{
    if (!s) {
        return;
    }
#ifdef LH_AMIGA
    s->lh_fh = 0;
    s->mem = NULL;
    s->mem_len = 0;
    s->mem_pos = 0;
    s->mem_owned = 0;
#elif defined(LH_HOST)
    s->lh_fp = NULL;
#endif
}

int lh_stream_open_mem(lh_stream *s, const void *data, unsigned long len,
    int owned)
{
    if (!s || !data || len == 0) {
        return 0;
    }
#ifdef LH_AMIGA
    s->lh_fh = 0;
    s->mem = (const unsigned char *)data;
    s->mem_len = len;
    s->mem_pos = 0;
    s->mem_owned = owned ? 1 : 0;
    return 1;
#else
    (void)owned;
    return 0;
#endif
}

int lh_stream_open_read(lh_stream *s, const char *path)
{
    if (!s || !path) {
        return 0;
    }
#ifdef LH_AMIGA
    /*
     * Handler may leave AllocVec'd archive bytes on LHBase so Open/Read never
     * WaitPkt on the LHA: process MsgPort during catalog or extract.
     */
    if (LhBase != NULL && LhBase->lhb_PendingMem != NULL
        && LhBase->lhb_PendingMemLen > 0) {
        APTR mem;
        ULONG len;

        mem = LhBase->lhb_PendingMem;
        len = LhBase->lhb_PendingMemLen;
        LhBase->lhb_PendingMem = NULL;
        LhBase->lhb_PendingMemLen = 0;
        (void)path;
        return lh_stream_open_mem(s, mem, len, 1);
    }
    s->lh_fh = Open((STRPTR)path, MODE_OLDFILE);
    return s->lh_fh != 0;
#elif defined(LH_HOST)
    s->lh_fp = fopen(path, "rb");
    return s->lh_fp != NULL;
#else
    return 0;
#endif
}

void lh_stream_close(lh_stream *s)
{
    if (!s) {
        return;
    }
#ifdef LH_AMIGA
    if (s->lh_fh) {
        Close(s->lh_fh);
        s->lh_fh = 0;
    }
    if (s->mem != NULL && s->mem_owned) {
        FreeVec((APTR)s->mem);
    }
    s->mem = NULL;
    s->mem_len = 0;
    s->mem_pos = 0;
    s->mem_owned = 0;
#elif defined(LH_HOST)
    if (s->lh_fp) {
        fclose(s->lh_fp);
        s->lh_fp = NULL;
    }
#endif
}

size_t lh_stream_read(lh_stream *s, void *buf, size_t n)
{
#ifdef LH_AMIGA
    LONG got;
    unsigned long left;
    unsigned long take;
#endif

    if (!s || !buf || n == 0) {
        return 0;
    }
#ifdef LH_AMIGA
    if (s->mem != NULL) {
        if (s->mem_pos >= s->mem_len) {
            return 0;
        }
        left = s->mem_len - s->mem_pos;
        take = (unsigned long)n;
        if (take > left) {
            take = left;
        }
        memcpy(buf, s->mem + s->mem_pos, (size_t)take);
        s->mem_pos += take;
        return (size_t)take;
    }
    if (!s->lh_fh) {
        return 0;
    }
    got = Read(s->lh_fh, buf, (LONG)n);
    if (got < 0) {
        return 0;
    }
    return (size_t)got;
#elif defined(LH_HOST)
    if (!s->lh_fp) {
        return 0;
    }
    return fread(buf, 1, n, s->lh_fp);
#else
    return 0;
#endif
}

int lh_stream_seek_cur(lh_stream *s, long delta)
{
    if (!s) {
        return 0;
    }
#ifdef LH_AMIGA
    if (s->mem != NULL) {
        long np;

        np = (long)s->mem_pos + delta;
        if (np < 0 || (unsigned long)np > s->mem_len) {
            return 0;
        }
        s->mem_pos = (unsigned long)np;
        return 1;
    }
    if (!s->lh_fh) {
        return 0;
    }
    return Seek(s->lh_fh, (LONG)delta, OFFSET_CURRENT) != -1L;
#elif defined(LH_HOST)
    if (!s->lh_fp) {
        return 0;
    }
    return fseek(s->lh_fp, delta, SEEK_CUR) == 0;
#else
    return 0;
#endif
}

int lh_stream_seek_set(lh_stream *s, long pos)
{
    if (!s || pos < 0) {
        return 0;
    }
#ifdef LH_AMIGA
    if (s->mem != NULL) {
        if ((unsigned long)pos > s->mem_len) {
            return 0;
        }
        s->mem_pos = (unsigned long)pos;
        return 1;
    }
    if (!s->lh_fh) {
        return 0;
    }
    return Seek(s->lh_fh, (LONG)pos, OFFSET_BEGINNING) != -1L;
#elif defined(LH_HOST)
    if (!s->lh_fp) {
        return 0;
    }
    return fseek(s->lh_fp, pos, SEEK_SET) == 0;
#else
    return 0;
#endif
}

int lh_stream_rewind(lh_stream *s)
{
    return lh_stream_seek_set(s, 0L);
}

long lh_stream_tell(const lh_stream *s)
{
    if (!s) {
        return -1L;
    }
#ifdef LH_AMIGA
    if (s->mem != NULL) {
        return (long)s->mem_pos;
    }
    if (!s->lh_fh) {
        return -1L;
    }
    return Seek(s->lh_fh, 0L, OFFSET_CURRENT);
#elif defined(LH_HOST)
    if (!s->lh_fp) {
        return -1L;
    }
    return ftell(s->lh_fp);
#else
    return -1L;
#endif
}

int lh_stream_open_write(lh_stream *s, const char *path)
{
#ifdef LH_AMIGA
    BPTR fh;
    LONG ioerr;
    int retry;
    char aside[560];
    LONG plen;
#endif

    if (!s || !path) {
        return 0;
    }
#ifdef LH_AMIGA
    for (retry = 0; retry < 4; retry++) {
        SetIoErr(0);
        fh = Open((STRPTR)path, MODE_OLDFILE);
        if (fh != (BPTR)NULL) {
            Close(fh);
        }
        DeleteFile((STRPTR)path);
        SetIoErr(0);
        s->lh_fh = Open((STRPTR)path, MODE_NEWFILE);
        if (s->lh_fh != (BPTR)NULL) {
            SetIoErr(0);
            return 1;
        }
        ioerr = IoErr();
        if (ioerr != ERROR_OBJECT_EXISTS) {
            break;
        }
    }
    plen = (LONG)strlen(path);
    if (plen > 0 && plen < (LONG)sizeof(aside) - 4) {
        strcpy(aside, path);
        strcat(aside, ".$$");
        SetIoErr(0);
        fh = Open((STRPTR)path, MODE_OLDFILE);
        if (fh != (BPTR)NULL) {
            Close(fh);
        }
        if (Rename((STRPTR)path, (STRPTR)aside)) {
            DeleteFile((STRPTR)aside);
        }
        SetIoErr(0);
        s->lh_fh = Open((STRPTR)path, MODE_NEWFILE);
        if (s->lh_fh != (BPTR)NULL) {
            SetIoErr(0);
            return 1;
        }
    }
    return 0;
#elif defined(LH_HOST)
    (void)unlink(path);
    s->lh_fp = fopen(path, "wb");
    return s->lh_fp != NULL;
#else
    return 0;
#endif
}

int lh_stream_open_append(lh_stream *s, const char *path)
{
    if (!s || !path) {
        return 0;
    }
#ifdef LH_AMIGA
    s->lh_fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!s->lh_fh) {
        return 0;
    }
    if (Seek(s->lh_fh, 0L, OFFSET_END) == -1L) {
        Close(s->lh_fh);
        s->lh_fh = 0;
        return 0;
    }
    return 1;
#elif defined(LH_HOST)
    s->lh_fp = fopen(path, "ab");
    return s->lh_fp != NULL;
#else
    return 0;
#endif
}

int lh_stream_file_exists(const char *path)
{
#ifdef LH_AMIGA
    BPTR lock;

    if (!path) {
        return 0;
    }
    if (LhBase != NULL && LhBase->lhb_PendingMem != NULL) {
        return 1;
    }
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock == ZERO) {
        return 0;
    }
    UnLock(lock);
    return 1;
#elif defined(LH_HOST)
    FILE *fp;

    if (!path) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    fclose(fp);
    return 1;
#else
    return 0;
#endif
}

size_t lh_stream_write(lh_stream *s, const void *buf, size_t n)
{
#ifdef LH_AMIGA
    LONG wrote;
#endif

    if (!s || !buf || n == 0) {
        return 0;
    }
#ifdef LH_AMIGA
    if (!s->lh_fh) {
        return 0;
    }
    wrote = Write(s->lh_fh, (APTR)buf, (LONG)n);
    if (wrote < 0) {
        return 0;
    }
    return (size_t)wrote;
#elif defined(LH_HOST)
    if (!s->lh_fp) {
        return 0;
    }
    return fwrite(buf, 1, n, s->lh_fp);
#else
    return 0;
#endif
}

struct lh_writer {
    lh_stream io;
    unsigned char header_level;
    lh_level default_level;
};

struct lh_reader {
    lh_stream io;
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
#ifdef LH_AMIGA
    if (path && lh_capture_mtime_amiga(path, &r->archive_dt)) {
        r->has_archive_dt = 1;
    }
#elif defined(LH_HOST)
    {
        struct stat st;

        if (r->io.lh_fp && fstat(fileno(r->io.lh_fp), &st) == 0) {
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
    if (level == LH_LEVEL_LHD) {
        return LH_METHOD_LHD;
    }
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
    size_t flen;
    size_t clen;

    memset(entry, 0, sizeof(*entry));
    if (meta->filename) {
        flen = strlen(meta->filename);
        entry->filename = (char *)malloc(flen + 1);
        if (entry->filename) {
            memcpy(entry->filename, meta->filename, flen + 1);
        }
    }
    if (meta->comment) {
        clen = strlen(meta->comment);
        entry->comment = (char *)malloc(clen + 1);
        if (entry->comment) {
            memcpy(entry->comment, meta->comment, clen + 1);
        }
    }
    entry->attrs = meta->attribute;
    if (meta->header_level >= 2) {
        lh_datetime_from_unix(&entry->datetime, meta->timestamp);
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
    lh_stream_init(&w->io);
    if (!lh_stream_open_write(&w->io, path)) {
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

    if (lh_stream_file_exists(path)) {
        w = (lh_writer *)calloc(1, sizeof(*w));
        if (!w) {
            if (err) {
                *err = LH_ERR_NO_MEMORY;
            }
            return NULL;
        }
        lh_stream_init(&w->io);
        if (!lh_stream_open_append(&w->io, path)) {
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
    lh_datetime now;

    if (!w || !LH_STREAM_OPEN(&w->io) || !filename) {
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
    if (comment && comment[0]) {
        meta.comment = (char *)comment;
    }
    if (datetime) {
        if (w->header_level >= 2) {
            meta.timestamp = lh_datetime_to_unix(datetime);
        } else {
            st = lh_datetime_pack(datetime, &ts);
            if (st != LH_OK) {
                return st;
            }
            meta.timestamp = ts;
        }
    } else {
        lh_datetime_now(&now);
        if (w->header_level >= 2) {
            meta.timestamp = lh_datetime_to_unix(&now);
        } else {
            meta.timestamp = lh_dos_timestamp_pack(&now);
        }
    }
    if (method == LH_METHOD_LHD) {
        meta.packed_size = 0;
        meta.is_directory = 1;
        strncpy(meta.method_sig, LH_SIG_LHD, LH_SIG_LEN);
        meta.method = LH_METHOD_LHD;
        meta.crc = 0;
        st = lh_hdr_write(&w->io, &meta, w->header_level);
        return st;
    }
    if (data_len == 0) {
        /* Empty regular file: -lh0- (or chosen method) with zero payload. */
        meta.packed_size = 0;
        meta.crc = 0;
        meta.has_crc = 1;
        st = lh_hdr_write(&w->io, &meta, w->header_level);
        return st;
    }
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
    st = lh_hdr_write(&w->io, &meta, w->header_level);
    if (st != LH_OK) {
        free(packed);
        return st;
    }
    if (packed_len > 0 && lh_stream_write(&w->io, packed, packed_len) != packed_len) {
        free(packed);
        return LH_ERR_IO;
    }
    free(packed);
    return LH_OK;
}

lh_status lh_writer_close(lh_writer **w)
{
    if (!w || !*w) {
        return LH_ERR_INVALID_ARG;
    }
    lh_stream_close(&(*w)->io);
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
    lh_stream_init(&r->io);
    if (!lh_stream_open_read(&r->io, path)) {
        free(r);
        if (err) {
            *err = LH_ERR_IO;
        }
        return NULL;
    }
    /* Memory-backed open skips Lock (avoids WaitPkt on handler port). */
#ifdef LH_AMIGA
    if (r->io.mem == NULL) {
        lh_reader_capture_mtime(r, path);
    }
#else
    lh_reader_capture_mtime(r, path);
#endif
    if (err) {
        *err = LH_OK;
    }
    return r;
}

lh_reader *lh_reader_open_mem(const void *data, unsigned long len, int owned,
    lh_status *err)
{
    lh_reader *r;

    if (!data || len == 0) {
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
    lh_stream_init(&r->io);
    if (!lh_stream_open_mem(&r->io, data, len, owned)) {
        free(r);
        if (err) {
            *err = LH_ERR_IO;
        }
        return NULL;
    }
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
    st = lh_hdr_read(&r->io, &meta, 0);
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
    if (meta.is_directory) {
        lh_hdr_meta_clear(&meta);
        return LH_OK;
    }
    /*
     * lh_meta_to_entry() already copied original_size into entry->data_len.
     * Only an empty stored file (original_size==0) has no payload after the
     * header.  packed_size==0 with original_size>0 is never valid LHA; treat
     * it as a header parse error instead of returning OK with no data buffer.
     */
    if (meta.original_size == 0) {
        lh_hdr_meta_clear(&meta);
        return LH_OK;
    }
    if (meta.packed_size == 0) {
        lh_hdr_meta_clear(&meta);
        lh_entry_clear(entry);
        return LH_ERR_BAD_HEADER;
    }
    if (r->header_only) {
        /*
         * Skip packed payload.  Encrypted archives must still run the stream
         * cipher over those bytes so a later decompress stays in sync.
         */
        if (r->has_password) {
            packed = (unsigned char *)malloc(meta.packed_size);
            if (!packed) {
                lh_hdr_meta_clear(&meta);
                lh_entry_clear(entry);
                return LH_ERR_NO_MEMORY;
            }
            n = lh_stream_read(&r->io, packed, meta.packed_size);
            if (n != meta.packed_size) {
                free(packed);
                lh_hdr_meta_clear(&meta);
                lh_entry_clear(entry);
                return LH_ERR_TRUNCATED;
            }
            lh_decrypt_buffer(&r->decrypt, packed, meta.packed_size);
            free(packed);
            packed = NULL;
        } else if (!lh_stream_seek_cur(&r->io, (long)meta.packed_size)) {
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
    n = lh_stream_read(&r->io, packed, meta.packed_size);
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

int lh_reader_rewind(lh_reader *r)
{
    if (!r) {
        return 0;
    }
    if (!lh_stream_rewind(&r->io)) {
        return 0;
    }
    /* Catalog walks set eof; must clear or lh_reader_next stays at EOF. */
    r->eof = 0;
    if (r->has_password) {
        lh_decrypt_init(&r->decrypt, r->password);
    }
    return 1;
}

long lh_reader_tell(const lh_reader *r)
{
    if (!r) {
        return -1L;
    }
    return lh_stream_tell(&r->io);
}

int lh_reader_seek(lh_reader *r, long pos)
{
    if (!r) {
        return 0;
    }
    if (!lh_stream_seek_set(&r->io, pos)) {
        return 0;
    }
    r->eof = 0;
    /*
     * Decrypt is a stream cipher from the archive start.  Callers that seek
     * mid-file with a password must walk/decrypt prior payloads themselves;
     * this only re-inits (correct after rewind/seek-to-0).
     */
    if (r->has_password) {
        lh_decrypt_init(&r->decrypt, r->password);
    }
    return 1;
}

void lh_reader_close(lh_reader **r)
{
    if (!r || !*r) {
        return;
    }
    lh_stream_close(&(*r)->io);
    free(*r);
    *r = NULL;
}

lh_status lh_archive_rewrite(
    const char *archive,
    lh_keep_fn keep,
    void *ctx,
    unsigned char header_level,
    lh_level level,
    int store_only
)
{
    char temp[512];
    lh_reader *r;
    lh_writer *w;
    lh_entry entry;
    lh_status st;
    lh_status err;

    if (!archive || !keep) {
        return LH_ERR_INVALID_ARG;
    }
    strncpy(temp, archive, sizeof(temp) - 6);
    temp[sizeof(temp) - 6] = '\0';
    strcat(temp, ".$$$");

    r = lh_reader_open(archive, &err);
    if (!r) {
        return err;
    }
    w = lh_writer_open(temp, header_level, level, &err);
    if (!w) {
        lh_reader_close(&r);
        return err;
    }
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(r, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            lh_entry_clear(&entry);
            break;
        }
        if (keep(&entry, ctx)) {
            st = lh_writer_add(w, entry.filename, entry.comment, entry.attrs,
                &entry.datetime, level, store_only,
                entry.data, entry.data_len);
            lh_entry_clear(&entry);
            if (st != LH_OK) {
                break;
            }
        } else {
            lh_entry_clear(&entry);
        }
    }
    lh_reader_close(&r);
    lh_writer_close(&w);
    if (st != LH_OK && st != LH_ERR_TRUNCATED) {
#ifdef LH_AMIGA
        DeleteFile((STRPTR)temp);
#elif defined(LH_HOST)
        remove(temp);
#endif
        return st;
    }
#ifdef LH_AMIGA
    DeleteFile((STRPTR)archive);
    if (Rename((STRPTR)temp, (STRPTR)archive) == DOSFALSE) {
        return LH_ERR_IO;
    }
#elif defined(LH_HOST)
    remove(archive);
    if (rename(temp, archive) != 0) {
        return LH_ERR_IO;
    }
#endif
    return LH_OK;
}
