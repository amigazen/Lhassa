/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_archive_dos.c - DOS.library-style archive access for lh.library.
 *
 * lh_arc_Archive acts as a volume root; lh_arc_Lock/lh_arc_examine/lh_arc_exnext mirror dos.library
 * directory traversal.  lh_arc_open_from_lock/lh_arc_read/lh_arc_closefh mirror file I/O on entries.
 */

#include "lh_native_guard.h"

#include <string.h>
#include <stddef.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/exall.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

#ifndef ST_FILE
#define ST_FILE (-3L)
#endif
#ifndef ST_USERDIR
#define ST_USERDIR (2L)
#endif

#include "/include/lh.h"
#include "lh/lhbase.h"
#include "lh_archive_dos.h"

/* Pool allocators from malloc.c (not in libc headers on SAS/C). */
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t num, size_t size);
void *realloc(void *ptr, size_t size);

extern struct LHBase *LhBase;

static void lh_arc_seterr(LONG code)
{
    if (LhBase) {
        LhBase->lhb_Err = code;
    }
}

static void lh_arc_clearerr(void)
{
    if (LhBase) {
        LhBase->lhb_Err = 0;
    }
}

/* MODE_NEWFILE needs an absent path; rename aside if DeleteFile fails.
 * Never operate on a volume/assign root (e.g. RAM: / SYS:) — DeleteFile or
 * Open(MODE_NEWFILE) there can destroy the volume's contents.
 */
static int lh_path_is_volume_root(STRPTR path)
{
    LONG len;
    STRPTR fp;

    if (!path || !path[0]) {
        return 1;
    }
    len = 0;
    while (path[len] != '\0') {
        len++;
    }
    if (path[len - 1] == ':') {
        return 1;
    }
    fp = FilePart(path);
    if (!fp || !fp[0]) {
        return 1;
    }
    return 0;
}

static void lh_arc_remove_path(STRPTR path)
{
    BPTR fh;
    LONG retry;
    char aside[560];
    LONG plen;

    if (!path || !path[0]) {
        return;
    }
    if (lh_path_is_volume_root(path)) {
        return;
    }
    for (retry = 0; retry < 4; retry++) {
        SetIoErr(0);
        fh = Open(path, MODE_OLDFILE);
        if (fh != (BPTR)NULL) {
            Close(fh);
        }
        DeleteFile(path);
        SetIoErr(0);
        fh = Open(path, MODE_OLDFILE);
        if (fh == (BPTR)NULL) {
            if (IoErr() == ERROR_OBJECT_NOT_FOUND || IoErr() == 0) {
                return;
            }
        } else {
            Close(fh);
        }
    }
    plen = (LONG)strlen((const char *)path);
    if (plen <= 0 || plen >= (LONG)sizeof(aside) - 4) {
        return;
    }
    strcpy(aside, (const char *)path);
    strcat(aside, ".$$");
    SetIoErr(0);
    fh = Open(path, MODE_OLDFILE);
    if (fh != (BPTR)NULL) {
        Close(fh);
    }
    if (Rename(path, (STRPTR)aside)) {
        DeleteFile((STRPTR)aside);
    }
    SetIoErr(0);
}

#ifndef OFFSET_BEGINNING
#define OFFSET_BEGINNING (-1L)
#define OFFSET_CURRENT   0L
#define OFFSET_END       1L
#endif

struct lh_arc_entry_ref {
    char *name;
    char *comment;
    unsigned long packed;
    unsigned long orig;
    lh_method method;
    unsigned char attrs;
    lh_datetime dt;
    int is_directory;
    unsigned short crc;
};

struct LhArchive {
    char path[512];
    LONG mode;
    lh_reader *reader;
    lh_writer *writer;
    struct lh_arc_entry_ref *catalog;
    LONG catalog_count;
    char password[256];
    int has_password;
    lh_datetime archive_dt;
    int has_archive_dt;
    /* AllocVec'd .lha image (handler); extract rewinds this, never path Open. */
    unsigned char *file_mem;
    unsigned long file_mem_len;
};

struct LhLock {
    struct LhArchive *archive;
    LONG entry_index;   /* -1 = archive root, -2 = synthetic dir, >=0 = entry */
    LONG iterate_index;
    char name[512];
    int exall_aborted;
};

#define LH_LOCK_ROOT_DIR   (-1L)
#define LH_LOCK_SYNTH_DIR  (-2L)

struct LhFileHandle {
    struct LhLock *lock;
    int owns_lock;
    unsigned char *data;
    unsigned long size;
    unsigned long pos;
    /* Always 0 today: handles are read-only memory images.  See lh_arc_write. */
    LONG writable;
};

static void lh_arc_clear_catalog(struct LhArchive *arc)
{
    LONG i;

    if (!arc || !arc->catalog) {
        return;
    }
    for (i = 0; i < arc->catalog_count; i++) {
        free(arc->catalog[i].name);
        free(arc->catalog[i].comment);
    }
    free(arc->catalog);
    arc->catalog = NULL;
    arc->catalog_count = 0;
}

static int lh_arc_grow_catalog(struct LhArchive *arc)
{
    struct lh_arc_entry_ref *n;
    LONG ncap;

    ncap = arc->catalog_count + 16;
    n = (struct lh_arc_entry_ref *)realloc(
        arc->catalog, (size_t)ncap * sizeof(struct lh_arc_entry_ref));
    if (!n) {
        return 0;
    }
    arc->catalog = n;
    return 1;
}

static int lh_arc_catalog_add(struct LhArchive *arc, const lh_entry *entry)
{
    struct lh_arc_entry_ref *ref;
    char *name_copy;
    char *comment_copy;

    if (!arc || !entry || !entry->filename) {
        return 0;
    }
    if ((arc->catalog_count % 16) == 0 && arc->catalog_count > 0) {
        if (!lh_arc_grow_catalog(arc)) {
            return 0;
        }
    }
    if (arc->catalog_count == 0 && !arc->catalog) {
        arc->catalog = (struct lh_arc_entry_ref *)calloc(
            16, sizeof(struct lh_arc_entry_ref));
        if (!arc->catalog) {
            return 0;
        }
    }
    name_copy = (char *)malloc(strlen(entry->filename) + 1);
    if (!name_copy) {
        return 0;
    }
    strcpy(name_copy, entry->filename);
    comment_copy = NULL;
    if (entry->comment && entry->comment[0]) {
        comment_copy = (char *)malloc(strlen(entry->comment) + 1);
        if (!comment_copy) {
            free(name_copy);
            return 0;
        }
        strcpy(comment_copy, entry->comment);
    }
    ref = &arc->catalog[arc->catalog_count];
    ref->name = name_copy;
    ref->comment = comment_copy;
    ref->packed = (unsigned long)entry->packed_len;
    ref->orig = (unsigned long)entry->data_len;
    ref->method = entry->method;
    ref->attrs = entry->attrs;
    ref->dt = entry->datetime;
    ref->is_directory = entry->is_directory;
    ref->crc = entry->crc;
    arc->catalog_count++;
    return 1;
}

static int lh_arc_build_catalog(struct LhArchive *arc)
{
    lh_entry entry;
    lh_status st;

    if (!arc || !arc->reader) {
        return 0;
    }
    lh_arc_clear_catalog(arc);
    lh_reader_set_header_only(arc->reader, 1);
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(arc->reader, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            lh_entry_clear(&entry);
            return 0;
        }
        if (!lh_arc_catalog_add(arc, &entry)) {
            lh_entry_clear(&entry);
            return 0;
        }
        lh_entry_clear(&entry);
    }
    lh_reader_set_header_only(arc->reader, 0);
    return 1;
}

static void lh_dt_to_datestamp(const lh_datetime *dt, struct DateStamp *ds)
{
    if (!dt || !ds) {
        return;
    }
    lh_datetime_to_datestamp(dt, ds);
}

static void lh_ref_to_fib(const struct lh_arc_entry_ref *ref, struct FileInfoBlock *fib)
{
    if (!ref || !fib) {
        return;
    }
    memset(fib, 0, sizeof(*fib));
    /*
     * LhExamine/LhExNext match dos.library Examine(): NUL-terminated C
     * strings.  Handlers filling ACTION_EXAMINE_* packets must use BSTRs
     * instead (RKRM 14.3.1); lh-handler converts before replying.
     */
    strncpy(fib->fib_FileName, ref->name, sizeof(fib->fib_FileName) - 1);
    fib->fib_FileName[sizeof(fib->fib_FileName) - 1] = '\0';
    fib->fib_Size = (LONG)ref->orig;
    fib->fib_NumBlocks = (LONG)((ref->orig + 511UL) / 512UL);
    /* Amiga protection bits are stored directly as the LHA attribute byte. */
    fib->fib_Protection = (LONG)ref->attrs;
    if (ref->is_directory) {
        /* RKRM: prefer ST_USERDIR; any positive value is a directory. */
        fib->fib_DirEntryType = ST_USERDIR;
        fib->fib_EntryType = ST_USERDIR;
    } else {
        fib->fib_DirEntryType = (LONG)ST_FILE;
        fib->fib_EntryType = (LONG)ST_FILE;
    }
    lh_dt_to_datestamp(&ref->dt, &fib->fib_Date);
    fib->fib_Comment[0] = '\0';
    if (ref->comment && ref->comment[0]) {
        strncpy(fib->fib_Comment, ref->comment, sizeof(fib->fib_Comment) - 1);
        fib->fib_Comment[sizeof(fib->fib_Comment) - 1] = '\0';
    }
}

static int lh_name_match(const char *pattern, const char *name)
{
    if (!pattern || !pattern[0] || strcmp(pattern, "*") == 0) {
        return 1;
    }
    return strcmp(pattern, name) == 0;
}

static LONG lh_find_entry_index(struct LhArchive *arc, const char *name)
{
    LONG i;

    if (!arc || !name) {
        return -1;
    }
    for (i = 0; i < arc->catalog_count; i++) {
        if (lh_name_match(name, arc->catalog[i].name)) {
            return i;
        }
    }
    return -1;
}

/*
 * Case-insensitive length of a shared path prefix (AmigaFS style).
 * Returns -1 if strings differ before either ends at a boundary.
 */
static LONG lh_path_prefix_len(const char *parent, const char *name)
{
    LONG i;

    if (!parent || !name) {
        return -1;
    }
    if (parent[0] == '\0') {
        return 0;
    }
    for (i = 0; parent[i] != '\0'; i++) {
        char a;
        char b;

        a = parent[i];
        b = name[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return -1;
        }
    }
    if (name[i] != '\0' && name[i] != '/') {
        return -1;
    }
    return i;
}

/* True if any catalog entry lives under parent/ (parent may be ""). */
static int lh_has_children_under(struct LhArchive *arc, const char *parent)
{
    LONG i;
    LONG plen;

    if (!arc || !parent) {
        return 0;
    }
    plen = (LONG)strlen(parent);
    for (i = 0; i < arc->catalog_count; i++) {
        const char *n;

        n = arc->catalog[i].name;
        if (!n) {
            continue;
        }
        if (plen == 0) {
            return 1;
        }
        if (lh_path_prefix_len(parent, n) == plen && n[plen] == '/') {
            return 1;
        }
    }
    return 0;
}

/*
 * First path component of name relative to parent into leaf[].
 * Returns 1 and sets *is_dir if this entry implies a directory child
 * (deeper path or explicit directory).  *exact_idx is catalog index when
 * the child name equals parent/leaf exactly, else -1.
 */
static int lh_child_leaf_from_entry(const char *parent, const char *name,
    char *leaf, LONG leafmax, int *is_dir, LONG entry_idx, LONG *exact_idx)
{
    LONG plen;
    LONG i;
    LONG j;
    const char *rest;

    if (!name || !leaf || leafmax <= 1 || !is_dir || !exact_idx) {
        return 0;
    }
    *exact_idx = -1;
    *is_dir = 0;
    plen = parent && parent[0] ? (LONG)strlen(parent) : 0;
    if (plen > 0) {
        if (lh_path_prefix_len(parent, name) != plen || name[plen] != '/') {
            return 0;
        }
        rest = name + plen + 1;
    } else {
        rest = name;
    }
    if (rest[0] == '\0') {
        return 0;
    }
    j = 0;
    for (i = 0; rest[i] != '\0' && rest[i] != '/' && j < leafmax - 1; i++) {
        leaf[j++] = rest[i];
    }
    leaf[j] = '\0';
    if (leaf[0] == '\0') {
        return 0;
    }
    if (rest[i] == '/') {
        *is_dir = 1;
    }
    if (rest[i] == '\0') {
        *exact_idx = entry_idx;
    }
    return 1;
}

/* True if leaf was already emitted from an earlier catalog entry under parent. */
static int lh_leaf_seen_before(struct LhArchive *arc, const char *parent,
    const char *leaf, LONG before_idx)
{
    LONG i;
    char prev[108];
    int is_dir;
    LONG exact;

    for (i = 0; i < before_idx; i++) {
        if (!lh_child_leaf_from_entry(parent, arc->catalog[i].name,
                prev, (LONG)sizeof(prev), &is_dir, i, &exact)) {
            continue;
        }
        if (lh_name_match(leaf, prev)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Advance iterate_index to the next unique immediate child under parent.
 * Fills fib with leaf name; uses catalog metadata when an exact entry exists.
 */
static int lh_fill_next_child(struct LhLock *lock, struct FileInfoBlock *fib)
{
    struct LhArchive *arc;
    const char *parent;
    LONG i;
    char leaf[108];
    int is_dir;
    LONG exact;
    struct lh_arc_entry_ref synth;

    if (!lock || !fib || !lock->archive) {
        return 0;
    }
    arc = lock->archive;
    if (lock->entry_index == LH_LOCK_ROOT_DIR) {
        parent = "";
    } else {
        parent = lock->name;
    }
    for (i = lock->iterate_index + 1; i < arc->catalog_count; i++) {
        if (!lh_child_leaf_from_entry(parent, arc->catalog[i].name,
                leaf, (LONG)sizeof(leaf), &is_dir, i, &exact)) {
            continue;
        }
        if (lh_leaf_seen_before(arc, parent, leaf, i)) {
            continue;
        }
        lock->iterate_index = i;
        if (exact >= 0) {
            if (arc->catalog[exact].is_directory) {
                is_dir = 1;
            }
            lh_ref_to_fib(&arc->catalog[exact], fib);
            strncpy(fib->fib_FileName, leaf, sizeof(fib->fib_FileName) - 1);
            fib->fib_FileName[sizeof(fib->fib_FileName) - 1] = '\0';
            if (is_dir) {
                fib->fib_DirEntryType = ST_USERDIR;
                fib->fib_EntryType = ST_USERDIR;
            }
            return 1;
        }
        /* Implied directory (only deeper paths exist). */
        memset(&synth, 0, sizeof(synth));
        synth.name = leaf;
        synth.is_directory = 1;
        synth.orig = 0;
        lh_ref_to_fib(&synth, fib);
        return 1;
    }
    return 0;
}

static int lh_read_entry_at_index(struct LhArchive *arc, LONG index,
    unsigned char **out, unsigned long *out_len, int *crc_ok)
{
    lh_entry entry;
    lh_status st;
    LONG i;

    if (!arc || !arc->reader || index < 0 || !out || !out_len) {
        return 0;
    }
    if (arc->has_password) {
        lh_reader_set_password(arc->reader, arc->password);
    }
    lh_reader_set_header_only(arc->reader, 0);
    for (i = 0; ; i++) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(arc->reader, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            lh_entry_clear(&entry);
            return 0;
        }
        if (i == index) {
            if (entry.is_directory || !entry.data || entry.data_len == 0) {
                lh_entry_clear(&entry);
                return 0;
            }
            *out = entry.data;
            *out_len = (unsigned long)entry.data_len;
            entry.data = NULL;
            if (crc_ok) {
                *crc_ok = entry.crc_ok;
            }
            lh_entry_clear(&entry);
            return 1;
        }
        lh_entry_clear(&entry);
    }
    return 0;
}

static int lh_read_entry_data(struct LhArchive *arc, const char *name,
    unsigned char **out, unsigned long *out_len, int *crc_ok)
{
    lh_entry entry;
    lh_status st;

    if (!arc || !arc->reader || !name || !out || !out_len) {
        return 0;
    }
    if (arc->has_password) {
        lh_reader_set_password(arc->reader, arc->password);
    }
    lh_reader_set_header_only(arc->reader, 0);
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(arc->reader, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            lh_entry_clear(&entry);
            return 0;
        }
        if (strcmp(entry.filename, name) == 0) {
            if (entry.is_directory) {
                *out = NULL;
                *out_len = 0;
                if (crc_ok) {
                    *crc_ok = 1;
                }
                lh_entry_clear(&entry);
                return 1;
            }
            if (!entry.data || entry.data_len == 0) {
                lh_entry_clear(&entry);
                return 0;
            }
            *out = entry.data;
            *out_len = (unsigned long)entry.data_len;
            entry.data = NULL;
            if (crc_ok) {
                *crc_ok = entry.crc_ok;
            }
            lh_entry_clear(&entry);
            return 1;
        }
        lh_entry_clear(&entry);
    }
    return 0;
}

static void lh_arc_drop_file_mem(struct LhArchive *arc)
{
    if (!arc || !arc->file_mem) {
        return;
    }
    FreeVec(arc->file_mem);
    arc->file_mem = NULL;
    arc->file_mem_len = 0;
}

static void lh_reopen_reader(struct LhArchive *arc)
{
    lh_status err;

    if (!arc) {
        return;
    }
    if (arc->reader) {
        /*
         * Rewind in place.  Close+path-reopen WaitPkts on the handler port
         * under heavy traffic; memory-backed archives never need path Open.
         */
        if (lh_reader_rewind(arc->reader)) {
            return;
        }
        lh_reader_close(&arc->reader);
    }
    if (arc->file_mem != NULL && arc->file_mem_len > 0) {
        arc->reader = lh_reader_open_mem(arc->file_mem, arc->file_mem_len, 0,
            &err);
    } else {
        arc->reader = lh_reader_open(arc->path, &err);
    }
    if (arc->reader && arc->has_password) {
        lh_reader_set_password(arc->reader, arc->password);
    }
}

struct LhArchive *lh_arc_open(STRPTR path, LONG mode)
{
    struct LhArchive *arc;
    lh_status err;

    if (!path || !path[0]) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return NULL;
    }
    arc = (struct LhArchive *)calloc(1, sizeof(*arc));
    if (!arc) {
        lh_arc_seterr(ERROR_NO_FREE_STORE);
        return NULL;
    }
    strncpy(arc->path, (const char *)path, sizeof(arc->path) - 1);
    arc->path[sizeof(arc->path) - 1] = '\0';
    arc->mode = mode;
    if (mode == LHARC_MODE_WRITE) {
        lh_arc_remove_path(path);
        arc->writer = lh_writer_open(arc->path, 2, LH_LEVEL_LH5, &err);
        if (!arc->writer) {
            free(arc);
            if (err == LH_ERR_NO_MEMORY) {
                lh_arc_seterr(ERROR_NO_FREE_STORE);
            } else if (IoErr() != 0) {
                lh_arc_seterr(IoErr());
            } else {
                lh_arc_seterr(ERROR_OBJECT_IN_USE);
            }
            return NULL;
        }
        lh_arc_clearerr();
        SetIoErr(0);
        return arc;
    }
    if (mode == LHARC_MODE_APPEND) {
        arc->writer = lh_writer_open_append(arc->path, &err);
        if (!arc->writer) {
            free(arc);
            lh_arc_seterr(IoErr() != 0 ? IoErr() : ERROR_OBJECT_IN_USE);
            return NULL;
        }
        return arc;
    }
    /*
     * Prefer pending memory image from the handler (private-reply load).
     * owned=0: FreeVec on lh_arc_close via file_mem.
     */
    if (LhBase != NULL && LhBase->lhb_PendingMem != NULL
        && LhBase->lhb_PendingMemLen > 0) {
        arc->file_mem = (unsigned char *)LhBase->lhb_PendingMem;
        arc->file_mem_len = LhBase->lhb_PendingMemLen;
        LhBase->lhb_PendingMem = NULL;
        LhBase->lhb_PendingMemLen = 0;
        arc->reader = lh_reader_open_mem(arc->file_mem, arc->file_mem_len, 0,
            &err);
    } else {
        arc->reader = lh_reader_open(arc->path, &err);
    }
    if (!arc->reader) {
        lh_arc_drop_file_mem(arc);
        free(arc);
        lh_arc_seterr(ERROR_OBJECT_NOT_FOUND);
        return NULL;
    }
    if (!lh_arc_build_catalog(arc)) {
        lh_reader_close(&arc->reader);
        lh_arc_drop_file_mem(arc);
        free(arc);
        lh_arc_seterr(ERROR_READ_PROTECTED);
        return NULL;
    }
    lh_reopen_reader(arc);
    if (lh_reader_archive_datetime(arc->reader, &arc->archive_dt)) {
        arc->has_archive_dt = 1;
    }
    return arc;
}

LONG lh_arc_close(struct LhArchive *archive)
{
    if (!archive) {
        return DOSFALSE;
    }
    if (archive->reader) {
        lh_reader_close(&archive->reader);
    }
    if (archive->writer) {
        lh_writer_close(&archive->writer);
    }
    lh_arc_clear_catalog(archive);
    lh_arc_drop_file_mem(archive);
    free(archive);
    return DOSTRUE;
}

BPTR lh_arc_lock(struct LhArchive *archive, STRPTR name)
{
    struct LhLock *lock;
    LONG idx;

    if (!archive) {
        lh_arc_seterr(ERROR_OBJECT_NOT_FOUND);
        return (BPTR)NULL;
    }
    lock = (struct LhLock *)calloc(1, sizeof(*lock));
    if (!lock) {
        lh_arc_seterr(ERROR_NO_FREE_STORE);
        return (BPTR)NULL;
    }
    lock->archive = archive;
    lock->iterate_index = -1;
    if (!name || !name[0]) {
        lock->entry_index = LH_LOCK_ROOT_DIR;
        lock->name[0] = '\0';
    } else {
        idx = lh_find_entry_index(archive, (const char *)name);
        strncpy(lock->name, (const char *)name, sizeof(lock->name) - 1);
        lock->name[sizeof(lock->name) - 1] = '\0';
        if (idx >= 0) {
            lock->entry_index = idx;
        } else if (lh_has_children_under(archive, (const char *)name)) {
            /*
             * Implied directory: path exists only as a prefix of other
             * entries (common when LHA omits explicit directory headers).
             */
            lock->entry_index = LH_LOCK_SYNTH_DIR;
        } else {
            free(lock);
            lh_arc_seterr(ERROR_OBJECT_NOT_FOUND);
            return (BPTR)NULL;
        }
    }
    return (BPTR)lock;
}

LONG lh_arc_unlock(BPTR lock_bptr)
{
    struct LhLock *lock;

    lock = (struct LhLock *)lock_bptr;
    if (!lock) {
        return DOSFALSE;
    }
    free(lock);
    return DOSTRUE;
}

LONG lh_arc_examine(BPTR lock_bptr, struct FileInfoBlock *fib)
{
    struct LhLock *lock;
    struct LhArchive *arc;
    struct lh_arc_entry_ref synth;
    const char *leaf;
    LONG i;

    if (!lock_bptr || !fib) {
        return DOSFALSE;
    }
    lock = (struct LhLock *)lock_bptr;
    arc = lock->archive;
    if (!arc) {
        return DOSFALSE;
    }
    if (lock->entry_index == LH_LOCK_ROOT_DIR) {
        /*
         * Archive root as a real directory: Examine fills the dir itself
         * (not the first catalog entry).  LhExNext then yields immediate
         * children with leaf names (test/test1/file -> "test").
         */
        memset(fib, 0, sizeof(*fib));
        fib->fib_DirEntryType = ST_USERDIR;
        fib->fib_EntryType = ST_USERDIR;
        fib->fib_FileName[0] = '\0';
        fib->fib_NumBlocks = 1;
        lock->iterate_index = -1;
        return DOSTRUE;
    }
    if (lock->entry_index == LH_LOCK_SYNTH_DIR) {
        leaf = lock->name;
        for (i = 0; lock->name[i] != '\0'; i++) {
            if (lock->name[i] == '/' || lock->name[i] == ':') {
                leaf = &lock->name[i + 1];
            }
        }
        memset(&synth, 0, sizeof(synth));
        synth.name = (char *)leaf;
        synth.is_directory = 1;
        lh_ref_to_fib(&synth, fib);
        lock->iterate_index = -1;
        return DOSTRUE;
    }
    if (lock->entry_index < 0 || lock->entry_index >= arc->catalog_count) {
        return DOSFALSE;
    }
    lh_ref_to_fib(&arc->catalog[lock->entry_index], fib);
    if (arc->catalog[lock->entry_index].is_directory) {
        lock->iterate_index = -1;
    }
    return DOSTRUE;
}

LONG lh_arc_exnext(BPTR lock_bptr, struct FileInfoBlock *fib)
{
    struct LhLock *lock;
    struct LhArchive *arc;

    if (!lock_bptr || !fib) {
        return DOSFALSE;
    }
    lock = (struct LhLock *)lock_bptr;
    arc = lock->archive;
    if (!arc) {
        return DOSFALSE;
    }

    /* Directory lock (root, synth, or explicit): immediate leaf children. */
    if (lock->entry_index == LH_LOCK_ROOT_DIR
        || lock->entry_index == LH_LOCK_SYNTH_DIR) {
        if (lh_fill_next_child(lock, fib)) {
            return DOSTRUE;
        }
        return DOSFALSE;
    }
    if (lock->entry_index >= 0
        && lock->entry_index < arc->catalog_count
        && arc->catalog[lock->entry_index].is_directory) {
        if (lh_fill_next_child(lock, fib)) {
            return DOSTRUE;
        }
        return DOSFALSE;
    }

    return DOSFALSE;
}

static ULONG lh_exall_hdrsize(LONG type)
{
    switch (type) {
    case ED_NAME:
        return 8UL;
    case ED_TYPE:
        return 12UL;
    case ED_SIZE:
        return 16UL;
    case ED_PROTECTION:
        return 20UL;
    case ED_DATE:
        return 32UL;
    case ED_COMMENT:
        return 36UL;
    default:
        return 0UL;
    }
}

static ULONG lh_exall_reclen(LONG type, const char *name, const char *comment)
{
    ULONG hdr;
    ULONG nlen;
    ULONG clen;
    ULONG total;

    hdr = lh_exall_hdrsize(type);
    if (hdr == 0UL || !name) {
        return 0UL;
    }
    nlen = (ULONG)strlen(name) + 1UL;
    total = hdr + nlen;
    if (total & 1UL) {
        total++;
    }
    if (type >= ED_COMMENT) {
        clen = 1UL;
        if (comment && comment[0]) {
            clen = (ULONG)strlen(comment) + 1UL;
        }
        total += clen;
        if (total & 1UL) {
            total++;
        }
    }
    return total;
}

static UBYTE *lh_exall_strptr(UBYTE *base, ULONG hdrsize)
{
    UBYTE *p;

    p = base + hdrsize;
    if (((ULONG)p) & 1UL) {
        p++;
    }
    return p;
}

static LONG lh_exall_accept(struct ExAllControl *ctl, struct ExAllData *ead, LONG type)
{
    struct Hook *hk;
    LONG (*func)(struct Hook *, struct ExAllData *, LONG *);

    if (!ctl || !ead) {
        return 0;
    }
    if (ctl->eac_MatchString && ead->ed_Name) {
        if (!MatchPatternNoCase(ctl->eac_MatchString, ead->ed_Name)) {
            return 0;
        }
    }
    hk = ctl->eac_MatchFunc;
    if (hk && hk->h_Entry) {
        func = (LONG (*)(struct Hook *, struct ExAllData *, LONG *))hk->h_Entry;
        return func(hk, ead, &type) ? 1 : 0;
    }
    return 1;
}

LONG lh_arc_exall(BPTR lock_bptr, STRPTR buffer, LONG buf_size, LONG type,
    struct ExAllControl *control)
{
    struct LhLock *lock;
    struct LhArchive *arc;
    struct ExAllData *ead;
    struct ExAllData *prev;
    struct lh_arc_entry_ref *ref;
    struct lh_arc_entry_ref synth;
    UBYTE *bp;
    UBYTE *bufend;
    UBYTE *strp;
    ULONG hdrsize;
    ULONG reclen;
    LONG idx;
    LONG entries;
    LONG exact;
    int is_dir;
    const char *parent;
    const char *comment;
    char leaf[108];
    struct DateStamp ds;
    int is_dirlock;

    if (!lock_bptr || !buffer || buf_size <= 0 || !control) {
        lh_arc_seterr(ERROR_NO_FREE_STORE);
        return DOSFALSE;
    }
    if (type < ED_NAME || type > ED_COMMENT) {
        lh_arc_seterr(ERROR_BAD_NUMBER);
        return DOSFALSE;
    }
    hdrsize = lh_exall_hdrsize(type);
    lock = (struct LhLock *)lock_bptr;
    arc = lock->archive;
    if (!arc) {
        lh_arc_seterr(ERROR_OBJECT_NOT_FOUND);
        return DOSFALSE;
    }

    /*
     * Nested virtual dirs: ExAll lists immediate children (leaf names),
     * same as LhExNext.  Flat catalog names are not exposed here.
     */
    is_dirlock = 0;
    if (lock->entry_index == LH_LOCK_ROOT_DIR
        || lock->entry_index == LH_LOCK_SYNTH_DIR) {
        is_dirlock = 1;
    } else if (lock->entry_index >= 0
        && lock->entry_index < arc->catalog_count
        && arc->catalog[lock->entry_index].is_directory) {
        is_dirlock = 1;
    }
    if (!is_dirlock) {
        lh_arc_seterr(ERROR_OBJECT_WRONG_TYPE);
        return DOSFALSE;
    }
    if (lock->exall_aborted) {
        lock->exall_aborted = 0;
        lh_arc_seterr(ERROR_NO_MORE_ENTRIES);
        control->eac_Entries = 0;
        return DOSFALSE;
    }

    if (lock->entry_index == LH_LOCK_ROOT_DIR) {
        parent = "";
    } else {
        parent = lock->name;
    }

    bp = (UBYTE *)buffer;
    bufend = bp + (ULONG)buf_size;
    prev = NULL;
    entries = 0;
    /* LastKey = next catalog index to scan (0 on first call). */
    idx = (LONG)control->eac_LastKey;

    while (idx < arc->catalog_count) {
        if (!lh_child_leaf_from_entry(parent, arc->catalog[idx].name,
                leaf, (LONG)sizeof(leaf), &is_dir, idx, &exact)) {
            idx++;
            continue;
        }
        if (lh_leaf_seen_before(arc, parent, leaf, idx)) {
            idx++;
            continue;
        }

        comment = NULL;
        ref = NULL;
        if (exact >= 0) {
            ref = &arc->catalog[exact];
            if (ref->is_directory) {
                is_dir = 1;
            }
            comment = ref->comment;
        } else {
            memset(&synth, 0, sizeof(synth));
            synth.name = leaf;
            synth.is_directory = 1;
            synth.orig = 0;
            ref = &synth;
            is_dir = 1;
        }

        reclen = lh_exall_reclen(type, leaf, comment);
        if (reclen == 0UL || bp + reclen > bufend) {
            if (entries == 0) {
                lh_arc_seterr(ERROR_LINE_TOO_LONG);
                control->eac_Entries = 0;
                control->eac_LastKey = (ULONG)idx;
                return DOSFALSE;
            }
            break;
        }
        ead = (struct ExAllData *)bp;
        memset(ead, 0, hdrsize);
        strp = lh_exall_strptr(bp, hdrsize);
        strcpy((char *)strp, leaf);
        ead->ed_Name = (UBYTE *)strp;
        if (type >= ED_TYPE) {
            ead->ed_Type = is_dir ? ST_USERDIR : ST_FILE;
        }
        if (type >= ED_SIZE) {
            ead->ed_Size = ref->orig;
        }
        if (type >= ED_PROTECTION) {
            ead->ed_Prot = (ULONG)ref->attrs;
        }
        if (type >= ED_DATE) {
            lh_dt_to_datestamp(&ref->dt, &ds);
            ead->ed_Days = (ULONG)ds.ds_Days;
            ead->ed_Mins = (ULONG)ds.ds_Minute;
            ead->ed_Ticks = (ULONG)ds.ds_Tick;
        }
        if (type >= ED_COMMENT) {
            UBYTE *cp;
            ULONG nlen;

            nlen = (ULONG)strlen(leaf) + 1UL;
            cp = strp + nlen;
            if (((ULONG)cp) & 1UL) {
                cp++;
            }
            if (comment && comment[0]) {
                strcpy((char *)cp, comment);
            } else {
                cp[0] = '\0';
            }
            ead->ed_Comment = cp;
        }
        if (!lh_exall_accept(control, ead, type)) {
            idx++;
            continue;
        }
        ead->ed_Next = NULL;
        if (prev) {
            prev->ed_Next = ead;
        }
        prev = ead;
        bp += reclen;
        entries++;
        idx++;
    }

    control->eac_Entries = (ULONG)entries;
    control->eac_LastKey = (ULONG)idx;
    if (idx >= arc->catalog_count) {
        lh_arc_seterr(ERROR_NO_MORE_ENTRIES);
        return DOSFALSE;
    }
    return DOSTRUE;
}

LONG lh_arc_exall_end(BPTR lock_bptr)
{
    struct LhLock *lock;

    lock = (struct LhLock *)lock_bptr;
    if (!lock) {
        return DOSFALSE;
    }
    lock->exall_aborted = 1;
    return DOSTRUE;
}

LONG lh_arc_info(BPTR lock_bptr, struct InfoData *info)
{
    struct LhLock *lock;
    struct LhArchive *arc;
    LONG i;
    unsigned long total;

    if (!lock_bptr || !info) {
        return DOSFALSE;
    }
    lock = (struct LhLock *)lock_bptr;
    arc = lock->archive;
    if (!arc) {
        return DOSFALSE;
    }
    memset(info, 0, sizeof(*info));
    total = 0;
    for (i = 0; i < arc->catalog_count; i++) {
        total += arc->catalog[i].orig;
    }
    info->id_DiskState = ID_VALIDATED;
    info->id_DiskType = ID_DOS_DISK;
    info->id_BytesPerBlock = 512;
    info->id_NumBlocks = (LONG)((total + 511UL) / 512UL);
    info->id_NumBlocksUsed = info->id_NumBlocks;
    info->id_InUse = DOSTRUE;
    return DOSTRUE;
}

LONG lh_arc_name_from_lock(BPTR lock_bptr, STRPTR buffer, LONG len)
{
    struct LhLock *lock;
    struct LhArchive *arc;

    if (!lock_bptr || !buffer || len <= 0) {
        return 0;
    }
    lock = (struct LhLock *)lock_bptr;
    arc = lock->archive;
    if (!arc) {
        buffer[0] = '\0';
        return 0;
    }
    if (lock->entry_index == LH_LOCK_ROOT_DIR) {
        strncpy(buffer, arc->path, (size_t)len - 1);
    } else {
        strncpy(buffer, lock->name, (size_t)len - 1);
    }
    buffer[len - 1] = '\0';
    return (LONG)strlen((char *)buffer);
}

BPTR lh_arc_open_from_lock(BPTR lock_bptr, LONG mode)
{
    struct LhLock *lock;
    struct LhArchive *arc;
    struct LhFileHandle *fh;
    int crc_ok;

    /*
     * Mode is ignored: every handle is a read-only decompressed image.
     * MODE_NEWFILE / MODE_READWRITE would be needed for a real LhWrite path
     * (see lh_arc_write).  Until then, open always loads the entry for read.
     */
    (void)mode;
    if (!lock_bptr) {
        return (BPTR)NULL;
    }
    lock = (struct LhLock *)lock_bptr;
    arc = lock->archive;
    if (!arc || lock->entry_index < 0) {
        return (BPTR)NULL;
    }
    if (arc->catalog[lock->entry_index].is_directory) {
        return (BPTR)NULL;
    }
    fh = (struct LhFileHandle *)calloc(1, sizeof(*fh));
    if (!fh) {
        return (BPTR)NULL;
    }
    fh->lock = lock;
    fh->owns_lock = 0;
    lh_reopen_reader(arc);
    if (!lh_read_entry_at_index(arc, lock->entry_index,
            &fh->data, &fh->size, &crc_ok)) {
        free(fh);
        return (BPTR)NULL;
    }
    /*
     * Reject handles with no payload when the catalog shows a non-empty file.
     * This catches stale lh.library builds and decompression failures that
     * would otherwise yield a zero-length LhRead loop.
     */
    if ((!fh->data || fh->size == 0)
        && lock->entry_index >= 0
        && lock->entry_index < arc->catalog_count
        && arc->catalog[lock->entry_index].orig > 0) {
        free(fh->data);
        free(fh);
        lh_arc_seterr(ERROR_OBJECT_WRONG_TYPE);
        return (BPTR)NULL;
    }
    fh->pos = 0;
    fh->writable = 0;
    lh_arc_clearerr();
    return (BPTR)fh;
}

static int lh_arc_mkdir_chain(STRPTR dirpath)
{
    char buf[512];
    char *p;
    BPTR lock;

    if (!dirpath || !dirpath[0]) {
        return 1;
    }
    strncpy(buf, (const char *)dirpath, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            lock = CreateDir((STRPTR)buf);
            if (lock == (BPTR)NULL && IoErr() != ERROR_OBJECT_EXISTS) {
                return 0;
            }
            if (lock != (BPTR)NULL) {
                UnLock(lock);
            }
            *p = '/';
        }
    }
    lock = CreateDir((STRPTR)buf);
    if (lock == (BPTR)NULL && IoErr() != ERROR_OBJECT_EXISTS) {
        return 0;
    }
    if (lock != (BPTR)NULL) {
        UnLock(lock);
    }
    return 1;
}

static int lh_arc_ensure_parent(STRPTR path)
{
    char buf[512];
    STRPTR pp;
    STRPTR p;

    if (!path || !path[0]) {
        return 0;
    }
    strncpy(buf, (const char *)path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pp = PathPart((STRPTR)buf);
    if (!pp || pp == (STRPTR)buf) {
        return 1;
    }
    *pp = '\0';
    if (buf[0] == '\0') {
        return 1;
    }
    p = (STRPTR)buf;
    while (*p) {
        p++;
    }
    if (p > (STRPTR)buf && *(p - 1) == ':') {
        return 1;
    }
    return lh_arc_mkdir_chain((STRPTR)buf);
}

/*
 * Decompress one entry.  Returns byte count in d0, or -1 on error.
 * *DataOut receives AllocMem data (caller FreeMem).  Size is only in d0
 * because SAS/C #pragma libcall does not reliably write a3 output args.
 */
LONG lh_arc_read_data(struct LhArchive *arc, STRPTR name, APTR *data_out)
{
    unsigned char *data;
    unsigned long sz;
    APTR copy;
    int crc_ok;

    if (!arc || !name || !data_out) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return -1L;
    }
    *data_out = NULL;
    lh_reopen_reader(arc);
    if (!lh_read_entry_data(arc, (const char *)name, &data, &sz, &crc_ok)) {
        lh_arc_seterr(ERROR_OBJECT_NOT_FOUND);
        return -1L;
    }
    if (!crc_ok) {
        if (data) {
            free(data);
        }
        lh_arc_seterr(ERROR_OBJECT_WRONG_TYPE);
        return -1L;
    }
    if (!data || sz == 0) {
        if (data) {
            free(data);
        }
        lh_arc_seterr(ERROR_OBJECT_WRONG_TYPE);
        return -1L;
    }
    copy = AllocMem((ULONG)sz, MEMF_ANY);
    if (!copy) {
        free(data);
        lh_arc_seterr(ERROR_NO_FREE_STORE);
        return -1L;
    }
    CopyMem((APTR)data, copy, (ULONG)sz);
    free(data);
    *data_out = copy;
    lh_arc_clearerr();
    return (LONG)sz;
}

LONG lh_arc_extract_entry(struct LhArchive *arc, STRPTR name, STRPTR dest)
{
    APTR data;
    LONG len;
    BPTR fh;
    LONG wrote;
    LONG idx;
    struct DateStamp ds;

    if (!arc || !name || !dest) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return DOSFALSE;
    }
    /* Refuse volume roots: Open(MODE_NEWFILE) on "RAM:" can wipe the disk. */
    if (lh_path_is_volume_root(dest)) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return DOSFALSE;
    }
    len = lh_arc_read_data(arc, name, &data);
    if (len < 0) {
        return DOSFALSE;
    }
    if (!lh_arc_ensure_parent(dest)) {
        FreeMem(data, (ULONG)len);
        lh_arc_seterr(ERROR_OBJECT_IN_USE);
        return DOSFALSE;
    }
    fh = Open(dest, MODE_NEWFILE);
    if (fh == (BPTR)NULL) {
        FreeMem(data, (ULONG)len);
        lh_arc_seterr(IoErr());
        return DOSFALSE;
    }
    wrote = Write(fh, data, len);
    Close(fh);
    FreeMem(data, (ULONG)len);
    if (wrote != len) {
        DeleteFile(dest);
        lh_arc_seterr(IoErr());
        return DOSFALSE;
    }
    /* Apply Amiga metadata from the catalog (protection, date, filenote). */
    idx = lh_find_entry_index(arc, (const char *)name);
    if (idx >= 0) {
        SetProtection(dest, (LONG)arc->catalog[idx].attrs);
        lh_datetime_to_datestamp(&arc->catalog[idx].dt, &ds);
        SetFileDate(dest, &ds);
        if (arc->catalog[idx].comment && arc->catalog[idx].comment[0]) {
            SetComment(dest, (STRPTR)arc->catalog[idx].comment);
        }
    }
    lh_arc_clearerr();
    return DOSTRUE;
}

LONG lh_arc_test_entry(struct LhArchive *arc, STRPTR name)
{
    APTR data;
    LONG len;

    len = lh_arc_read_data(arc, name, &data);
    if (len < 0) {
        return -1L;
    }
    FreeMem(data, (ULONG)len);
    return len;
}

LONG lh_arc_print_entry(struct LhArchive *arc, STRPTR name)
{
    APTR data;
    LONG len;
    BPTR outfh;
    LONG wrote;

    len = lh_arc_read_data(arc, name, &data);
    if (len < 0) {
        return DOSFALSE;
    }
    outfh = Output();
    if (outfh == (BPTR)NULL) {
        FreeMem(data, (ULONG)len);
        lh_arc_seterr(ERROR_OBJECT_IN_USE);
        return DOSFALSE;
    }
    if (len > 0) {
        wrote = Write(outfh, data, len);
        if (wrote != len) {
            FreeMem(data, (ULONG)len);
            lh_arc_seterr(IoErr());
            return DOSFALSE;
        }
    }
    FreeMem(data, (ULONG)len);
    lh_arc_clearerr();
    return DOSTRUE;
}

BPTR lh_arc_open_entry(struct LhArchive *archive, STRPTR name, LONG mode)
{
    BPTR lock;
    BPTR fh;

    lock = lh_arc_lock(archive, name);
    if (!lock) {
        return (BPTR)NULL;
    }
    fh = lh_arc_open_from_lock(lock, mode);
    if (!fh) {
        lh_arc_unlock(lock);
        return (BPTR)NULL;
    }
    ((struct LhFileHandle *)fh)->owns_lock = 1;
    return fh;
}

LONG lh_arc_read(BPTR fh_bptr, APTR buffer, LONG len)
{
    struct LhFileHandle *fh;
    unsigned long avail;
    unsigned long n;

    if (!fh_bptr || !buffer || len <= 0) {
        return -1;
    }
    fh = (struct LhFileHandle *)fh_bptr;
    if (!fh->data || fh->pos >= fh->size) {
        return 0;
    }
    avail = fh->size - fh->pos;
    n = (unsigned long)len;
    if (n > avail) {
        n = avail;
    }
    memcpy(buffer, fh->data + fh->pos, n);
    fh->pos += n;
    return (LONG)n;
}

/*
 * LhWrite is intentionally a stub: always returns -1.
 *
 * Why: entry handles are not streaming writers.  LhOpen / LhOpenFromLock
 * always decompress the whole entry into fh->data and set writable=0; mode
 * is ignored.  Creating or replacing entries is done with LhAddEntry (and
 * LhConcatArchive / LhDeleteFile for mutate) on an archive opened with
 * LHARC_MODE_WRITE or LHARC_MODE_APPEND -- not via LhRead/LhWrite handles.
 * The LVO exists for DOS-style API symmetry so read-only clients get a
 * clean rejection rather than a missing vector.
 *
 * How a working version could work:
 * 1. LhOpen/LhOpenFromLock honour Mode.  MODE_OLDFILE stays read-only as
 *    today.  MODE_NEWFILE / MODE_READWRITE require the parent archive to be
 *    open for write or append; set fh->writable and keep the entry name
 *    (and optional existing payload for read/write).
 * 2. LhWrite grows or overwrites fh->data (realloc), advances fh->pos and
 *    fh->size, returns bytes written.  Reject if !writable.
 * 3. LhClose on a writable handle commits by rewriting that entry in the
 *    archive (same rewrite path as LhDeleteFile / LhAddEntry: temp file,
 *    copy other entries, write this entry's buffer, rename).  Read-only
 *    close stays a free of the memory image only.
 * 4. Until that lands, callers must use LhAddEntry for new/replaced data.
 */
LONG lh_arc_write(BPTR fh_bptr, APTR buffer, LONG len)
{
    (void)fh_bptr;
    (void)buffer;
    (void)len;
    return -1;
}

LONG lh_arc_closefh(BPTR fh_bptr)
{
    struct LhFileHandle *fh;

    if (!fh_bptr) {
        return DOSFALSE;
    }
    fh = (struct LhFileHandle *)fh_bptr;
    /* Writable handles would commit fh->data into the archive here. */
    if (fh->lock && fh->owns_lock) {
        lh_arc_unlock((BPTR)fh->lock);
        fh->lock = NULL;
    }
    free(fh->data);
    free(fh);
    return DOSTRUE;
}

LONG lh_arc_seek(BPTR fh_bptr, LONG position, LONG mode)
{
    struct LhFileHandle *fh;
    unsigned long newpos;

    if (!fh_bptr) {
        return -1;
    }
    fh = (struct LhFileHandle *)fh_bptr;
    if (mode == OFFSET_BEGINNING) {
        newpos = (unsigned long)position;
    } else if (mode == OFFSET_CURRENT) {
        if (position < 0 && (unsigned long)(-position) > fh->pos) {
            return -1;
        }
        newpos = fh->pos + (unsigned long)position;
    } else if (mode == OFFSET_END) {
        if (position > 0) {
            return -1;
        }
        if ((unsigned long)(-position) > fh->size) {
            return -1;
        }
        newpos = fh->size - (unsigned long)(-position);
    } else {
        return -1;
    }
    if (newpos > fh->size) {
        return -1;
    }
    fh->pos = newpos;
    return (LONG)newpos;
}

LONG lh_arc_add_entry(struct LhArchive *archive, STRPTR name, APTR data, LONG len)
{
    return lh_arc_add_entry_taglist(archive, name, data, len, NULL);
}

/*
 * LhAddEntryTagList -- add with optional TagItem overrides.
 *
 * Defaults: header level 2 (writer), LH0/store, LH_ATTR_DEFAULT,
 * current DateStamp, no comment.  Store is the default until LH5
 * compress/decompress round-trips reliably (real LHA rejects bad LH5).
 * Request LH5 explicitly with LHADD_Method / LhAddEntryTags.
 *
 * Tags (libraries/lh.h):
 *   LHADD_Method     - lh_level (0=store, 5=lh5, 11=dir)
 *   LHADD_Attrs      - Amiga protection bits -> LHA attribute byte
 *   LHADD_DateStamp  - struct DateStamp *
 *   LHADD_Comment    - STRPTR filenote -> extension 0x41
 *   LHADD_Directory  - nonzero -> -lhd- directory (ignores data)
 */
LONG lh_arc_add_entry_taglist(
    struct LhArchive *archive,
    STRPTR name,
    APTR data,
    LONG len,
    struct TagItem *tags)
{
    lh_status st;
    lh_datetime dt;
    lh_level level;
    lh_attrs attrs;
    const char *comment;
    struct TagItem *t;
    struct DateStamp *ds;
    int as_dir;

    if (!archive || !archive->writer || !name) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return DOSFALSE;
    }
    if (len > 0 && data == NULL) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return DOSFALSE;
    }

    /* Default store; LHADD_Method selects lh5/etc when the codec is ready. */
    level = LH_LEVEL_STORE;
    attrs = LH_ATTR_DEFAULT;
    comment = NULL;
    as_dir = 0;
    lh_datetime_now(&dt);

    if (tags) {
        for (t = tags; t->ti_Tag != TAG_DONE; t++) {
            if (t->ti_Tag == TAG_IGNORE) {
                continue;
            }
            if (t->ti_Tag == LHADD_Method) {
                level = (lh_level)t->ti_Data;
            } else if (t->ti_Tag == LHADD_Attrs) {
                attrs = (lh_attrs)(t->ti_Data & 0xffUL);
            } else if (t->ti_Tag == LHADD_DateStamp) {
                ds = (struct DateStamp *)t->ti_Data;
                if (ds) {
                    lh_datetime_from_datestamp(ds, &dt);
                }
            } else if (t->ti_Tag == LHADD_Comment) {
                comment = (const char *)t->ti_Data;
            } else if (t->ti_Tag == LHADD_Directory) {
                if (t->ti_Data) {
                    as_dir = 1;
                }
            }
        }
    }
    if (as_dir) {
        level = LH_LEVEL_LHD;
        data = NULL;
        len = 0;
    }

    st = lh_writer_add(archive->writer, (const char *)name, comment,
        attrs, &dt, level, 0,
        (const unsigned char *)data, (size_t)len);
    if (st != LH_OK) {
        if (st == LH_ERR_NO_MEMORY) {
            lh_arc_seterr(ERROR_NO_FREE_STORE);
        } else if (st == LH_ERR_IO && IoErr() != 0) {
            lh_arc_seterr(IoErr());
        } else {
            lh_arc_seterr(ERROR_WRITE_PROTECTED);
        }
        return DOSFALSE;
    }
    lh_arc_clearerr();
    SetIoErr(0);
    return DOSTRUE;
}

static int lh_keep_not_name(const lh_entry *entry, void *ctx)
{
    const char *name;

    name = (const char *)ctx;
    if (!entry || !entry->filename || !name) {
        return 0;
    }
    return strcmp(entry->filename, name) != 0;
}

LONG lh_arc_delete_entry(struct LhArchive *archive, STRPTR name)
{
    lh_status st;
    int had_reader;

    if (!archive || !name) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return DOSFALSE;
    }
    had_reader = (archive->reader != NULL) ? 1 : 0;
    if (had_reader) {
        lh_reader_close(&archive->reader);
    }
    /* On-disk rewrite invalidates any memory image of the old file. */
    lh_arc_drop_file_mem(archive);
    st = lh_archive_rewrite(archive->path, lh_keep_not_name, (void *)name,
        2, LH_LEVEL_STORE, 1);
    if (st != LH_OK) {
        if (had_reader) {
            archive->reader = lh_reader_open(archive->path, &st);
            if (archive->reader) {
                lh_arc_build_catalog(archive);
                lh_reopen_reader(archive);
            }
        }
        if (st == LH_ERR_NO_MEMORY) {
            lh_arc_seterr(ERROR_NO_FREE_STORE);
        } else if (st == LH_ERR_IO && IoErr() != 0) {
            lh_arc_seterr(IoErr());
        } else {
            lh_arc_seterr(ERROR_WRITE_PROTECTED);
        }
        return DOSFALSE;
    }
    if (had_reader) {
        st = LH_OK;
        archive->reader = lh_reader_open(archive->path, &st);
        if (!archive->reader || !lh_arc_build_catalog(archive)) {
            lh_arc_seterr(ERROR_READ_PROTECTED);
            return DOSFALSE;
        }
        lh_reopen_reader(archive);
    }
    lh_arc_clearerr();
    SetIoErr(0);
    return DOSTRUE;
}

LONG lh_arc_concat(struct LhArchive *dest, STRPTR source_path)
{
    struct LhArchive *src;
    LONG i;
    unsigned char *data;
    unsigned long len;
    int crc_ok;
    struct TagItem tags[5];
    ULONG n;
    lh_level level;

    if (!dest || !dest->writer || !source_path) {
        lh_arc_seterr(ERROR_INVALID_COMPONENT_NAME);
        return DOSFALSE;
    }
    src = lh_arc_open(source_path, LHARC_MODE_READ);
    if (!src) {
        return DOSFALSE;
    }
    for (i = 0; i < src->catalog_count; i++) {
        if (src->catalog[i].is_directory) {
            continue;
        }
        lh_reopen_reader(src);
        data = NULL;
        len = 0;
        if (!lh_read_entry_data(src, src->catalog[i].name, &data, &len, &crc_ok)) {
            lh_arc_close(src);
            lh_arc_seterr(ERROR_OBJECT_NOT_FOUND);
            return DOSFALSE;
        }
        /*
         * Re-add with source metadata.  Map method; LH0 stays stored so
         * concat does not force a broken LH5 recompress of stored members.
         */
        if (src->catalog[i].method == LH_METHOD_LH0) {
            level = LH_LEVEL_STORE;
        } else if (src->catalog[i].method == LH_METHOD_LH1) {
            level = LH_LEVEL_LH1;
        } else if (src->catalog[i].method == LH_METHOD_LH6) {
            level = LH_LEVEL_LH6;
        } else if (src->catalog[i].method == LH_METHOD_LH7) {
            level = LH_LEVEL_LH7;
        } else {
            /* Prefer store until LH5 round-trip is reliable. */
            level = LH_LEVEL_STORE;
        }
        n = 0;
        tags[n].ti_Tag = LHADD_Method;
        tags[n].ti_Data = (ULONG)level;
        n++;
        tags[n].ti_Tag = LHADD_Attrs;
        tags[n].ti_Data = (ULONG)src->catalog[i].attrs;
        n++;
        tags[n].ti_Tag = LHADD_DateStamp;
        /* DateStamp tag wants struct DateStamp *; convert via stack. */
        {
            struct DateStamp ds;

            lh_datetime_to_datestamp(&src->catalog[i].dt, &ds);
            tags[n].ti_Data = (ULONG)&ds;
            n++;
            if (src->catalog[i].comment && src->catalog[i].comment[0]) {
                tags[n].ti_Tag = LHADD_Comment;
                tags[n].ti_Data = (ULONG)src->catalog[i].comment;
                n++;
            }
            tags[n].ti_Tag = TAG_DONE;
            if (!lh_arc_add_entry_taglist(dest, (STRPTR)src->catalog[i].name,
                    data, (LONG)len, tags)) {
                free(data);
                lh_arc_close(src);
                return DOSFALSE;
            }
        }
        free(data);
    }
    lh_arc_close(src);
    lh_arc_clearerr();
    return DOSTRUE;
}

LONG lh_arc_set_password(struct LhArchive *archive, STRPTR password)
{
    if (!archive) {
        return DOSFALSE;
    }
    archive->has_password = 0;
    archive->password[0] = '\0';
    if (password && password[0]) {
        strncpy(archive->password, (const char *)password,
            sizeof(archive->password) - 1);
        archive->password[sizeof(archive->password) - 1] = '\0';
        archive->has_password = 1;
        if (archive->reader) {
            lh_reader_set_password(archive->reader, archive->password);
        }
    }
    return DOSTRUE;
}
