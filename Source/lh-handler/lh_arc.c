/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_arc.c - Locks and handles: real FS passthrough + LHA archive overlay.
 */

#include "lh-handler.h"

int lhh_lock_valid(GD gd, struct LhHLock *lock)
{
    if (lock == NULL || gd == NULL) {
        return 0;
    }
    if (lock->magic != LHH_LOCK_MAGIC) {
        return 0;
    }
    if (lock->fl.fl_Task != gd->gd_Port) {
        return 0;
    }
    if (lock->type > LHH_LOCK_ENTRY) {
        return 0;
    }
    return 1;
}

void lhh_arc_init(GD gd)
{
    NewList((struct List *)&gd->gd_Arcs);
}

static struct LhHArc *lhh_arc_find(GD gd, const char *real_path)
{
    struct LhHArc *arc;

    for (arc = (struct LhHArc *)gd->gd_Arcs.mlh_Head;
         arc->node.mln_Succ != NULL;
         arc = (struct LhHArc *)arc->node.mln_Succ) {
        if (lhh_cstr_eq_i(arc->real_path, real_path)) {
            return arc;
        }
    }
    return NULL;
}

struct LhHArc *lhh_arc_obtain(GD gd, const char *real_path, LONG *err)
{
    struct LhHArc *ha;
    struct LhArchive *archive;

    ha = lhh_arc_find(gd, real_path);
    if (ha != NULL) {
        ha->refcount++;
        return ha;
    }

    archive = LhOpenArchive((STRPTR)real_path, LHARC_MODE_READ);
    if (archive == NULL) {
        if (err) {
            *err = LhErr();
            if (*err == 0) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
        }
        return NULL;
    }

    ha = (struct LhHArc *)AllocPooled(gd->gd_Pool, sizeof(struct LhHArc));
    if (ha == NULL) {
        LhCloseArchive(archive);
        if (err) {
            *err = ERROR_NO_FREE_STORE;
        }
        return NULL;
    }
    ha->archive = archive;
    ha->refcount = 1;
    lhh_cstr_copy(ha->real_path, real_path, LHH_PATH_LEN);
    AddTail((struct List *)&gd->gd_Arcs, (struct Node *)&ha->node);
    return ha;
}

void lhh_arc_release(GD gd, struct LhHArc *arc)
{
    if (arc == NULL) {
        return;
    }
    arc->refcount--;
    if (arc->refcount > 0) {
        return;
    }
    Remove((struct Node *)&arc->node);
    if (arc->archive != NULL) {
        LhCloseArchive(arc->archive);
        arc->archive = NULL;
    }
    FreePooled(gd->gd_Pool, arc, sizeof(struct LhHArc));
}

int lhh_arc_refresh(GD gd, struct LhHArc *arc, LONG *err)
{
    struct LhArchive *archive;

    if (arc == NULL) {
        return 0;
    }
    if (arc->archive != NULL) {
        LhCloseArchive(arc->archive);
        arc->archive = NULL;
    }
    archive = LhOpenArchive((STRPTR)arc->real_path, LHARC_MODE_READ);
    if (archive == NULL) {
        if (err) {
            *err = LhErr();
            if (*err == 0) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
        }
        return 0;
    }
    arc->archive = archive;
    return 1;
}

static void lhh_lock_init_fl(GD gd, struct LhHLock *lock, LONG access)
{
    lock->fl.fl_Link = ZERO;
    lock->fl.fl_Key = 1;
    lock->fl.fl_Access = access;
    lock->fl.fl_Task = gd->gd_Port;
    lock->fl.fl_Volume = MKBADDR(gd->gd_DosList);
    lock->access = access;
}

/*
 * type ROOT: arc/entry/real_lock ignored.
 * type REAL: real_lock + real_path required; consumes real_lock on success.
 * type ARCHIVE/ENTRY: arc required (consumed); entry for ENTRY; real_path set.
 */
struct LhHLock *lhh_lock_alloc(GD gd, ULONG type, struct LhHArc *arc,
    const char *entry, BPTR real_lock, const char *real_path,
    const char *virt_path, LONG access, LONG *err)
{
    struct LhHLock *lock;
    BPTR lh;
    STRPTR name;

    lock = (struct LhHLock *)AllocPooled(gd->gd_Pool, sizeof(struct LhHLock));
    if (lock == NULL) {
        if (arc != NULL) {
            lhh_arc_release(gd, arc);
        }
        if (real_lock != ZERO) {
            lhh_host_unlock(real_lock);
        }
        if (err) {
            *err = ERROR_NO_FREE_STORE;
        }
        return NULL;
    }
    lock->magic = LHH_LOCK_MAGIC;
    lock->type = type;
    lock->arc = NULL;
    lock->lh_lock = ZERO;
    lock->real_lock = ZERO;
    lock->entry[0] = '\0';
    lock->real_path[0] = '\0';
    lock->virt_path[0] = '\0';
    lhh_lock_init_fl(gd, lock, access);

    if (type == LHH_LOCK_ROOT) {
        if (arc != NULL) {
            lhh_arc_release(gd, arc);
        }
        if (real_lock != ZERO) {
            lhh_host_unlock(real_lock);
        }
        gd->gd_LockCnt++;
        gd->gd_UsageCnt++;
        return lock;
    }

    if (virt_path) {
        lhh_cstr_copy(lock->virt_path, virt_path, LHH_PATH_LEN);
    } else if (real_path && (type == LHH_LOCK_REAL || type == LHH_LOCK_ARCHIVE)) {
        lhh_real_to_virt_path(gd, real_path, lock->virt_path, LHH_PATH_LEN);
    }

    if (type == LHH_LOCK_REAL) {
        if (arc != NULL) {
            lhh_arc_release(gd, arc);
        }
        if (real_lock == ZERO) {
            FreePooled(gd->gd_Pool, lock, sizeof(struct LhHLock));
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        lock->real_lock = real_lock;
        if (real_path) {
            lhh_cstr_copy(lock->real_path, real_path, LHH_PATH_LEN);
        }
        gd->gd_LockCnt++;
        gd->gd_UsageCnt++;
        return lock;
    }

    /* ARCHIVE or ENTRY */
    if (real_lock != ZERO) {
        lhh_host_unlock(real_lock);
    }
    if (arc == NULL || arc->archive == NULL) {
        FreePooled(gd->gd_Pool, lock, sizeof(struct LhHLock));
        if (arc != NULL) {
            lhh_arc_release(gd, arc);
        }
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }
    lock->arc = arc;
    if (real_path) {
        lhh_cstr_copy(lock->real_path, real_path, LHH_PATH_LEN);
    } else {
        lhh_cstr_copy(lock->real_path, arc->real_path, LHH_PATH_LEN);
    }
    if (lock->virt_path[0] == '\0') {
        lhh_real_to_virt_path(gd, lock->real_path, lock->virt_path,
            LHH_PATH_LEN);
    }

    if (type == LHH_LOCK_ARCHIVE) {
        name = (STRPTR)"";
    } else {
        if (!entry || !entry[0]) {
            FreePooled(gd->gd_Pool, lock, sizeof(struct LhHLock));
            lhh_arc_release(gd, arc);
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        lhh_cstr_copy(lock->entry, entry, LHH_PATH_LEN);
        name = (STRPTR)lock->entry;
    }

    lh = LhLock(arc->archive, name);
    if (lh == ZERO) {
        FreePooled(gd->gd_Pool, lock, sizeof(struct LhHLock));
        lhh_arc_release(gd, arc);
        if (err) {
            *err = LhErr();
            if (*err == 0) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
        }
        return NULL;
    }
    lock->lh_lock = lh;
    gd->gd_LockCnt++;
    gd->gd_UsageCnt++;
    return lock;
}

void lhh_lock_free(GD gd, struct LhHLock *lock)
{
    if (!lhh_lock_valid(gd, lock)) {
        return;
    }
    if (lock->lh_lock != ZERO) {
        LhUnLock(lock->lh_lock);
        lock->lh_lock = ZERO;
    }
    if (lock->real_lock != ZERO) {
        lhh_host_unlock(lock->real_lock);
        lock->real_lock = ZERO;
    }
    if (lock->arc != NULL) {
        lhh_arc_release(gd, lock->arc);
        lock->arc = NULL;
    }
    lock->magic = 0;
    lock->fl.fl_Task = NULL;
    gd->gd_LockCnt--;
    gd->gd_UsageCnt--;
    FreePooled(gd->gd_Pool, lock, sizeof(struct LhHLock));
}

struct LhHLock *lhh_lock_dup(GD gd, struct LhHLock *src, LONG *err)
{
    BPTR dlock;
    struct LhHArc *arc;

    if (!lhh_lock_valid(gd, src)) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }
    if (src->type == LHH_LOCK_ROOT) {
        return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
            NULL, src->access, err);
    }
    if (src->type == LHH_LOCK_REAL) {
        dlock = lhh_host_duplock(src->real_lock);
        if (dlock == ZERO) {
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            return NULL;
        }
        return lhh_lock_alloc(gd, LHH_LOCK_REAL, NULL, NULL, dlock,
            src->real_path, src->virt_path, src->access, err);
    }
    arc = src->arc;
    if (arc == NULL) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }
    arc->refcount++;
    return lhh_lock_alloc(gd, src->type, arc, src->entry, ZERO,
        src->real_path, src->virt_path, src->access, err);
}

/*
 * Build a real Amiga path from parent lock + one name component.
 * Multi-component names must be split by lhh_lock_resolve before calling.
 */
static int lhh_build_real_path(GD gd, struct LhHLock *parent,
    const char *name, STRPTR out, LONG outlen)
{
    /* static: handler process stack is only a few KB */
    LONG n;

    out[0] = '\0';

    if (parent == NULL || parent->type == LHH_LOCK_ROOT) {
        if (!name || !name[0]) {
            return 1; /* volume root of LHA: */
        }
        if (lhh_is_parent_name(name)) {
            return 0;
        }
        /* One component: bare volume/assign name (RAM, AmigaZen, ...).
         * Names with ':' are invalid under LHA: (LHA:AmigaZen: fails). */
        if (lhh_cstr_has(name, '/') || lhh_cstr_has(name, ':')) {
            return 0;
        }
        if (!lhh_canon_volume(gd, name, out, outlen)) {
            return 0;
        }
        n = lhh_cstr_len(out);
        if (n + 1 < outlen) {
            out[n] = ':';
            out[n + 1] = '\0';
        }
        return 1;
    }

    if (parent->type == LHH_LOCK_REAL) {
        if (!name || !name[0]) {
            lhh_cstr_copy(out, parent->real_path, outlen);
            return 1;
        }
        if (lhh_is_parent_name(name)) {
            return 0;
        }
        if (lhh_cstr_has(name, '/') || lhh_cstr_has(name, ':')) {
            return 0;
        }
        lhh_real_append_name(out, outlen, parent->real_path, name);
        return out[0] ? 1 : 0;
    }

    return 0;
}

/*
 * Build virtual path under LHA: from parent lock + one name component.
 */
static int lhh_build_virt_path(GD gd, struct LhHLock *parent,
    const char *name, STRPTR out, LONG outlen)
{
    out[0] = '\0';

    if (parent == NULL || parent->type == LHH_LOCK_ROOT) {
        if (!name || !name[0]) {
            return 1;
        }
        if (lhh_is_parent_name(name)) {
            return 0;
        }
        /* Bare volume name only - no ':' in virtual paths. */
        if (lhh_cstr_has(name, '/') || lhh_cstr_has(name, ':')) {
            return 0;
        }
        if (!lhh_canon_volume(gd, name, out, outlen)) {
            return 0;
        }
        return 1;
    }

    if (parent->type == LHH_LOCK_REAL || parent->type == LHH_LOCK_ARCHIVE
        || parent->type == LHH_LOCK_ENTRY) {
        if (!name || !name[0]) {
            lhh_cstr_copy(out, parent->virt_path, outlen);
            return 1;
        }
        if (lhh_is_parent_name(name)) {
            return 0;
        }
        if (lhh_cstr_has(name, '/') || lhh_cstr_has(name, ':')) {
            return 0;
        }
        lhh_virt_append_name(out, outlen, parent->virt_path, name);
        return out[0] ? 1 : 0;
    }

    return 0;
}

static struct LhHLock *lhh_lock_real_path(GD gd, const char *real_path,
    const char *virt_path, LONG access, LONG *err);

struct LhHLock *lhh_lock_parent(GD gd, struct LhHLock *lock, LONG *err)
{
    static char ppath[LHH_PATH_LEN];
    static char pvirt[LHH_PATH_LEN];
    static char avirt[LHH_PATH_LEN];
    static char pentry[LHH_PATH_LEN];
    BPTR plock;
    LONG n;
    LONG alen;
    struct LhHArc *arc;
    LONG access;

    access = SHARED_LOCK;
    if (!lhh_lock_valid(gd, lock) || lock->type == LHH_LOCK_ROOT) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }

    if (lock->type == LHH_LOCK_REAL) {
        n = lhh_cstr_len(lock->real_path);
        if (n > 0 && lock->real_path[n - 1] == ':'
            && !lhh_cstr_has(lock->real_path, '/')) {
            return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
                NULL, access, err);
        }
        if (!lhh_virt_parent_path(lock->virt_path, pvirt, LHH_PATH_LEN)
            || !lhh_parent_path(lock->real_path, ppath, LHH_PATH_LEN)) {
            return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
                NULL, access, err);
        }
        plock = lhh_host_lock((STRPTR)ppath, access);
        if (plock == ZERO) {
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            return NULL;
        }
        return lhh_lock_alloc(gd, LHH_LOCK_REAL, NULL, NULL, plock, ppath,
            pvirt, access, err);
    }

    if (lock->type == LHH_LOCK_ENTRY) {
        if (!lhh_virt_parent_path(lock->virt_path, pvirt, LHH_PATH_LEN)) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        if (!lhh_real_to_virt_path(gd, lock->real_path, avirt, LHH_PATH_LEN)) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        arc = lock->arc;
        if (arc == NULL) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        arc->refcount++;
        alen = lhh_cstr_len(avirt);
        if (lhh_cstr_eq_i(pvirt, avirt)) {
            return lhh_lock_alloc(gd, LHH_LOCK_ARCHIVE, arc, NULL, ZERO,
                lock->real_path, avirt, access, err);
        }
        if (pvirt[alen] != '/') {
            lhh_arc_release(gd, arc);
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        lhh_cstr_copy(pentry, pvirt + alen + 1, LHH_PATH_LEN);
        return lhh_lock_alloc(gd, LHH_LOCK_ENTRY, arc, pentry, ZERO,
            lock->real_path, pvirt, access, err);
    }

    if (lock->type == LHH_LOCK_ARCHIVE) {
        if (!lhh_virt_parent_path(lock->virt_path, pvirt, LHH_PATH_LEN)
            || !lhh_parent_path(lock->real_path, ppath, LHH_PATH_LEN)) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        plock = lhh_host_lock((STRPTR)ppath, access);
        if (plock == ZERO) {
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            return NULL;
        }
        return lhh_lock_alloc(gd, LHH_LOCK_REAL, NULL, NULL, plock, ppath,
            pvirt, access, err);
    }

    if (err) {
        *err = ERROR_OBJECT_WRONG_TYPE;
    }
    return NULL;
}

static struct LhHLock *lhh_lock_resolve_one(GD gd, struct LhHLock *parent,
    const char *comp, LONG access, LONG *err)
{
    static char real[LHH_PATH_LEN];
    static char virt[LHH_PATH_LEN];
    BPTR rlock;
    struct FileInfoBlock *fib;
    struct LhHArc *ha;
    LONG is_file;

    if (lhh_is_parent_name(comp)) {
        return lhh_lock_parent(gd, parent, err);
    }
    if (!lhh_build_real_path(gd, parent, comp, real, LHH_PATH_LEN)
        || !lhh_build_virt_path(gd, parent, comp, virt, LHH_PATH_LEN)) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }
    if (real[0] == '\0') {
        return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
            NULL, access, err);
    }

    /*
     * Nested walk: locate relative to the parent's host lock.  Rebuilding
     * "Vol:dir/name" and locking from scratch fails on some assigns/volumes.
     */
    if (parent != NULL && parent->type == LHH_LOCK_REAL
        && parent->real_lock != ZERO) {
        rlock = lhh_host_lock_from(parent->real_lock, (STRPTR)comp, access);
        if (rlock == ZERO) {
            DB2("host_lock_from fail comp=%s err=%ld\n", comp, (LONG)IoErr());
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            return NULL;
        }

        fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (fib == NULL) {
            lhh_host_unlock(rlock);
            if (err) {
                *err = ERROR_NO_FREE_STORE;
            }
            return NULL;
        }
        is_file = 0;
        if (lhh_host_examine(rlock, fib)) {
            if (fib->fib_DirEntryType < 0) {
                is_file = 1;
            }
        }
        FreeMem(fib, sizeof(struct FileInfoBlock));

        if (is_file) {
            ha = lhh_arc_obtain(gd, real, err);
            if (ha != NULL) {
                lhh_host_unlock(rlock);
                return lhh_lock_alloc(gd, LHH_LOCK_ARCHIVE, ha, NULL, ZERO,
                    real, virt, access, err);
            }
            if (err) {
                *err = 0;
            }
        }
        return lhh_lock_alloc(gd, LHH_LOCK_REAL, NULL, NULL, rlock, real,
            virt, access, err);
    }

    return lhh_lock_real_path(gd, real, virt, access, err);
}

/*
 * Lock real_path on the host FS.  If it is an LHA archive file, return an
 * ARCHIVE lock; otherwise a REAL passthrough lock.
 */
static struct LhHLock *lhh_lock_real_path(GD gd, const char *real_path,
    const char *virt_path, LONG access, LONG *err)
{
    BPTR rlock;
    struct FileInfoBlock *fib;
    struct LhHArc *ha;
    LONG is_file;
    static char vbuf[LHH_PATH_LEN];

    if (!real_path || !real_path[0]) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }
    if (virt_path == NULL || virt_path[0] == '\0') {
        if (!lhh_real_to_virt_path(gd, real_path, vbuf, LHH_PATH_LEN)) {
            vbuf[0] = '\0';
        }
        virt_path = vbuf;
    }
    /*
     * "LHA:" / "LHA" is our volume root, not a host path.  Recursing into
     * LHA:foo is invalid; only the bare volume maps to ROOT.
     */
    if (lhh_is_self_real(gd, real_path)) {
        if (lhh_is_volume_root_name(gd, real_path)) {
            return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
                NULL, access, err);
        }
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }

    /* Host packets only - never dos.library Lock() on pr_MsgPort. */
    rlock = lhh_host_lock((STRPTR)real_path, access);
    if (rlock == ZERO) {
        DB2("host_lock fail real=%s err=%ld\n", real_path, (LONG)IoErr());
        if (err) {
            *err = IoErr();
            if (*err == 0) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
        }
        return NULL;
    }

    fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        lhh_host_unlock(rlock);
        if (err) {
            *err = ERROR_NO_FREE_STORE;
        }
        return NULL;
    }

    is_file = 0;
    if (lhh_host_examine(rlock, fib)) {
        if (fib->fib_DirEntryType < 0) {
            is_file = 1;
        }
    }
    FreeMem(fib, sizeof(struct FileInfoBlock));

    if (is_file) {
        /*
         * Try to present a valid LHA as a virtual directory.  If
         * LhOpenArchive fails (e.g. not an archive), keep REAL passthrough.
         */
        ha = lhh_arc_obtain(gd, real_path, err);
        if (ha != NULL) {
            lhh_host_unlock(rlock);
            return lhh_lock_alloc(gd, LHH_LOCK_ARCHIVE, ha, NULL, ZERO,
                real_path, virt_path, access, err);
        }
        if (err) {
            *err = 0;
        }
    }

    return lhh_lock_alloc(gd, LHH_LOCK_REAL, NULL, NULL, rlock, real_path,
        virt_path, access, err);
}

struct LhHLock *lhh_lock_resolve(GD gd, struct LhHLock *parent,
    STRPTR name_bstr, LONG access, LONG *err)
{
    /* static: must not blow the handler's tiny stack */
    static char name[LHH_PATH_LEN];
    static char comp[LHH_PATH_LEN];
    static char entry[LHH_PATH_LEN];
    const char *rest;
    struct LhHLock *lock;
    struct LhHArc *arc;
    LONG i;
    LONG j;

    lhh_bstr_to_cstr(name_bstr, name, LHH_PATH_LEN);
    /* Strip trailing '/' so "lhasa/" matches "lhasa". */
    {
        LONG n;

        n = lhh_cstr_len(name);
        while (n > 0 && name[n - 1] == '/') {
            name[n - 1] = '\0';
            n--;
        }
    }
    DB2("resolve name=%s parent=%s\n", name,
        parent ? lhh_lock_type_name(parent->type) : "NULL");

    /* Empty / self-volume name: lock the parent object (or LHA: root). */
    if (parent == NULL || parent->type == LHH_LOCK_ROOT) {
        if (lhh_is_volume_root_name(gd, name)) {
            return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
                NULL, access, err);
        }
    } else if (name[0] == '\0') {
        return lhh_lock_dup(gd, parent, err);
    }

    if (parent != NULL && parent->type == LHH_LOCK_ENTRY) {
        if (err) {
            *err = ERROR_OBJECT_WRONG_TYPE;
        }
        return NULL;
    }

    /* Inside an archive: remainder is the entry path (may contain '/'). */
    if (parent != NULL && parent->type == LHH_LOCK_ARCHIVE) {
        static char ventry[LHH_PATH_LEN];

        arc = parent->arc;
        if (arc == NULL) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        lhh_virt_append_name(ventry, LHH_PATH_LEN, parent->virt_path, name);
        arc->refcount++;
        return lhh_lock_alloc(gd, LHH_LOCK_ENTRY, arc, name, ZERO,
            parent->real_path, ventry, access, err);
    }

    /*
     * Walk one Amiga component at a time.  lha:amigazen/lhasa/file.lha/lha
     * becomes amigazen -> lhasa -> file.lha -> lha (entry after archive).
     */
    lock = parent;
    rest = name;
    while (rest != NULL && rest[0] != '\0') {
        if (lock != NULL && lock->type == LHH_LOCK_ARCHIVE) {
            break;
        }
        if (!lhh_path_next(&rest, comp, LHH_PATH_LEN)) {
            break;
        }
        lock = lhh_lock_resolve_one(gd, lock, comp, access, err);
        if (lock == NULL) {
            return NULL;
        }
    }

    if (lock != NULL && lock->type == LHH_LOCK_ARCHIVE
        && rest != NULL && rest[0] != '\0') {
        static char arcreal[LHH_PATH_LEN];
        static char ventry[LHH_PATH_LEN];

        arc = lock->arc;
        if (arc == NULL) {
            lhh_lock_free(gd, lock);
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        j = 0;
        for (i = 0; rest[i] != '\0' && j < LHH_PATH_LEN - 1; i++, j++) {
            entry[j] = rest[i];
        }
        entry[j] = '\0';
        lhh_cstr_copy(arcreal, lock->real_path, LHH_PATH_LEN);
        lhh_virt_append_name(ventry, LHH_PATH_LEN, lock->virt_path, entry);
        arc->refcount++;
        lhh_lock_free(gd, lock);
        return lhh_lock_alloc(gd, LHH_LOCK_ENTRY, arc, entry, ZERO,
            arcreal, ventry, access, err);
    }

    return lock;
}

static struct LhHHandle *lhh_handle_alloc(GD gd, LONG *err)
{
    struct LhHHandle *hh;

    hh = (struct LhHHandle *)AllocPooled(gd->gd_Pool, sizeof(struct LhHHandle));
    if (hh == NULL) {
        if (err) {
            *err = ERROR_NO_FREE_STORE;
        }
        return NULL;
    }
    hh->lock = NULL;
    hh->lh_fh = ZERO;
    hh->real_fh = ZERO;
    hh->writing = 0;
    hh->wbuf = NULL;
    hh->wlen = 0;
    hh->wcap = 0;
    hh->entry[0] = '\0';
    return hh;
}

void lhh_handle_close(GD gd, struct LhHHandle *hh)
{
    struct LhArchive *warc;
    LONG err;

    if (hh == NULL) {
        return;
    }

    if (hh->writing) {
        if (hh->lock != NULL && hh->lock->arc != NULL && hh->entry[0]) {
            warc = LhOpenArchive((STRPTR)hh->lock->arc->real_path,
                LHARC_MODE_APPEND);
            if (warc != NULL) {
                LhAddEntry(warc, (STRPTR)hh->entry,
                    hh->wbuf, (LONG)hh->wlen);
                LhCloseArchive(warc);
                lhh_arc_refresh(gd, hh->lock->arc, &err);
            }
        }
        if (hh->wbuf != NULL) {
            FreeMem(hh->wbuf, hh->wcap);
            hh->wbuf = NULL;
        }
    }

    if (hh->lh_fh != ZERO) {
        LhClose(hh->lh_fh);
        hh->lh_fh = ZERO;
    }
    if (hh->real_fh != ZERO) {
        lhh_host_close(hh->real_fh);
        hh->real_fh = ZERO;
    }
    if (hh->lock != NULL) {
        lhh_lock_free(gd, hh->lock);
        hh->lock = NULL;
    }
    gd->gd_UsageCnt--;
    FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
}

struct LhHHandle *lhh_handle_open(GD gd, struct LhHLock *parent,
    STRPTR name_bstr, LONG mode, LONG *err)
{
    struct LhHLock *lock;
    struct LhHHandle *hh;
    BPTR fh;
    static char name[LHH_PATH_LEN];
    LONG access;

    lhh_bstr_to_cstr(name_bstr, name, LHH_PATH_LEN);

    access = SHARED_LOCK;
    if (mode == MODE_NEWFILE || mode == MODE_READWRITE) {
        access = EXCLUSIVE_LOCK;
    }

    if (name[0] == '\0') {
        if (parent == NULL) {
            if (err) {
                *err = ERROR_OBJECT_WRONG_TYPE;
            }
            return NULL;
        }
        lock = lhh_lock_dup(gd, parent, err);
    } else {
        lock = lhh_lock_resolve(gd, parent, name_bstr, access, err);
    }

    /* New archive entry under ARCHIVE lock. */
    if (lock == NULL && (mode == MODE_NEWFILE || mode == MODE_READWRITE)
        && parent != NULL && parent->type == LHH_LOCK_ARCHIVE && name[0]) {
        lock = lhh_lock_dup(gd, parent, err);
        if (lock != NULL) {
            hh = lhh_handle_alloc(gd, err);
            if (hh == NULL) {
                lhh_lock_free(gd, lock);
                return NULL;
            }
            hh->lock = lock;
            hh->writing = 1;
            lhh_cstr_copy(hh->entry, name, LHH_PATH_LEN);
            gd->gd_UsageCnt++;
            return hh;
        }
    }

    if (lock == NULL) {
        return NULL;
    }

    hh = lhh_handle_alloc(gd, err);
    if (hh == NULL) {
        lhh_lock_free(gd, lock);
        return NULL;
    }
    hh->lock = lock;

    if (lock->type == LHH_LOCK_REAL) {
        fh = lhh_host_open((STRPTR)lock->real_path, mode);
        if (fh == ZERO) {
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            lhh_lock_free(gd, lock);
            FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
            return NULL;
        }
        hh->real_fh = fh;
        gd->gd_UsageCnt++;
        return hh;
    }

    if (lock->type == LHH_LOCK_ENTRY) {
        if (mode == MODE_NEWFILE || mode == MODE_READWRITE) {
            hh->writing = 1;
            lhh_cstr_copy(hh->entry, lock->entry, LHH_PATH_LEN);
            if (lock->arc != NULL) {
                LhDeleteFile(lock->arc->archive, (STRPTR)hh->entry);
                lhh_arc_refresh(gd, lock->arc, err);
            }
            gd->gd_UsageCnt++;
            return hh;
        }
        fh = LhOpenFromLock(lock->lh_lock, mode);
        if (fh == ZERO) {
            if (err) {
                *err = LhErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            lhh_lock_free(gd, lock);
            FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
            return NULL;
        }
        hh->lh_fh = fh;
        gd->gd_UsageCnt++;
        return hh;
    }

    lhh_lock_free(gd, lock);
    FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
    if (err) {
        *err = ERROR_OBJECT_WRONG_TYPE;
    }
    return NULL;
}

LONG lhh_handle_read(GD gd, struct LhHHandle *hh, APTR buf, LONG len)
{
    (void)gd;

    if (hh == NULL || hh->writing) {
        return -1;
    }
    if (hh->real_fh != ZERO) {
        return lhh_host_read(hh->real_fh, buf, len);
    }
    if (hh->lh_fh != ZERO) {
        return LhRead(hh->lh_fh, buf, len);
    }
    return -1;
}

LONG lhh_handle_write(GD gd, struct LhHHandle *hh, APTR buf, LONG len)
{
    UBYTE *nbuf;
    ULONG ncap;

    if (hh == NULL || !buf || len <= 0) {
        return -1;
    }
    if (hh->real_fh != ZERO) {
        return lhh_host_write(hh->real_fh, buf, len);
    }
    if (!hh->writing) {
        return -1;
    }
    if (hh->wlen + (ULONG)len > hh->wcap) {
        ncap = hh->wcap ? hh->wcap * 2 : 4096UL;
        while (ncap < hh->wlen + (ULONG)len) {
            ncap *= 2;
        }
        nbuf = (UBYTE *)AllocMem(ncap, MEMF_ANY);
        if (nbuf == NULL) {
            return -1;
        }
        if (hh->wbuf != NULL && hh->wlen > 0) {
            CopyMem(hh->wbuf, nbuf, hh->wlen);
            FreeMem(hh->wbuf, hh->wcap);
        }
        hh->wbuf = nbuf;
        hh->wcap = ncap;
    }
    CopyMem(buf, hh->wbuf + hh->wlen, (ULONG)len);
    hh->wlen += (ULONG)len;
    return len;
}

LONG lhh_handle_seek(GD gd, struct LhHHandle *hh, LONG pos, LONG mode)
{
    (void)gd;

    if (hh == NULL) {
        return -1;
    }
    if (hh->real_fh != ZERO) {
        return lhh_host_seek(hh->real_fh, pos, mode);
    }
    if (hh->writing) {
        LONG newpos;

        if (mode == OFFSET_BEGINNING) {
            newpos = pos;
        } else if (mode == OFFSET_CURRENT) {
            newpos = (LONG)hh->wlen + pos;
        } else if (mode == OFFSET_END) {
            newpos = (LONG)hh->wlen + pos;
        } else {
            return -1;
        }
        if (newpos < 0 || (ULONG)newpos > hh->wlen) {
            return -1;
        }
        return newpos;
    }
    if (hh->lh_fh != ZERO) {
        return LhSeek(hh->lh_fh, pos, mode);
    }
    return -1;
}

LONG lhh_delete_name(GD gd, struct LhHLock *parent, STRPTR name_bstr, LONG *err)
{
    struct LhHLock *lock;
    LONG ok;
    static char path[LHH_PATH_LEN];

    lock = lhh_lock_resolve(gd, parent, name_bstr, SHARED_LOCK, err);
    if (lock == NULL) {
        return DOSFALSE;
    }

    if (lock->type == LHH_LOCK_ENTRY && lock->arc != NULL) {
        ok = LhDeleteFile(lock->arc->archive, (STRPTR)lock->entry);
        if (!ok) {
            if (err) {
                *err = LhErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            lhh_lock_free(gd, lock);
            return DOSFALSE;
        }
        lhh_arc_refresh(gd, lock->arc, err);
        lhh_lock_free(gd, lock);
        return DOSTRUE;
    }

    if (lock->type == LHH_LOCK_REAL || lock->type == LHH_LOCK_ARCHIVE) {
        lhh_cstr_copy(path, lock->real_path, LHH_PATH_LEN);
        lhh_lock_free(gd, lock);
        ok = DeleteFile((STRPTR)path);
        if (!ok) {
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            return DOSFALSE;
        }
        return DOSTRUE;
    }

    lhh_lock_free(gd, lock);
    if (err) {
        *err = ERROR_OBJECT_WRONG_TYPE;
    }
    return DOSFALSE;
}
