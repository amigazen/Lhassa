/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_arc.c - Locks and handles: real FS passthrough + LHA archive overlay.
 */

#include "lh-handler.h"
#include "lh_drawer_info.h"

/* Default icons from GetDefDiskObject cache (lh_icon.c). */
static void lhh_set_icon_mem(struct LhHHandle *hh, int kind)
{
    hh->mem_data = lhh_icon_data(kind);
    hh->mem_len = lhh_icon_len(kind);
    hh->mem_pos = 0;
}

static int lhh_want_disk_icon(struct LhHLock *lock, const char *name)
{
    if (lock != NULL && lock->type == LHH_LOCK_VINFO
        && lock->vinfo_kind == LHH_VINFO_DISK) {
        return 1;
    }
    if (name != NULL && lhh_is_disk_info_name(name)) {
        return 1;
    }
    if (lock != NULL && lhh_is_disk_info_name(lock->virt_path)) {
        return 1;
    }
    return 0;
}

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
    if (lock->type > LHH_LOCK_VINFO) {
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
    /* fl_Key set later by lhh_lock_set_key() once paths are known. */
    lock->fl.fl_Key = 0;
    lock->fl.fl_Access = access;
    lock->fl.fl_Task = gd->gd_Port;
    /* RKRM: fl_Volume = BPTR to DLT_VOLUME, not the device node. */
    lock->fl.fl_Volume = lhh_volume_bptr(gd);
    lock->access = access;
}

/*
 * fl_Key identifies the object, not the lock instance.  dos.library
 * SameLock may fall back to comparing keys; WB FindParent uses
 * SameLock(MakeDiskLock(disk), parent) after ParentDir walks up from
 * the .lha.  Unique-per-alloc keys made every root lock look different.
 */
static LONG lhh_hash_path(const char *s)
{
    ULONG h;
    LONG i;
    char c;

    h = 5381UL;
    if (s == NULL) {
        return (LONG)h;
    }
    for (i = 0; s[i] != '\0'; i++) {
        c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        h = ((h << 5) + h) + (ULONG)(UBYTE)c;
    }
    return (LONG)h;
}

static void lhh_lock_set_key(struct LhHLock *lock)
{
    if (lock->type == LHH_LOCK_ROOT) {
        lock->fl.fl_Key = 1;
        return;
    }
    lock->fl.fl_Key = lhh_hash_path(lock->virt_path)
        ^ ((LONG)lock->type << 24)
        ^ lhh_hash_path(lock->entry);
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
    lock->pending_info[0] = '\0';
    lock->pending_diskkey = 0;
    lock->pending_date.ds_Days = 0;
    lock->pending_date.ds_Minute = 0;
    lock->pending_date.ds_Tick = 0;
    lock->vinfo_kind = LHH_VINFO_DRAWER;
    lhh_lock_init_fl(gd, lock, access);

    if (type == LHH_LOCK_ROOT) {
        if (arc != NULL) {
            lhh_arc_release(gd, arc);
        }
        if (real_lock != ZERO) {
            lhh_host_unlock(real_lock);
        }
        lhh_lock_set_key(lock);
        gd->gd_LockCnt++;
        gd->gd_UsageCnt++;
        return lock;
    }

    if (type == LHH_LOCK_VINFO) {
        if (arc != NULL) {
            lhh_arc_release(gd, arc);
        }
        if (real_lock != ZERO) {
            lhh_host_unlock(real_lock);
        }
        if (virt_path) {
            lhh_cstr_copy(lock->virt_path, virt_path, LHH_PATH_LEN);
        }
        /*
         * Volume Disk.info must be WBDISK (Workbench DiskInserted rejects
         * any other type).  Drawer *.info stays WBDRAWER.
         */
        if ((entry != NULL && entry[0] == 'D')
            || (virt_path != NULL && lhh_is_disk_info_name(virt_path))) {
            lock->vinfo_kind = LHH_VINFO_DISK;
        } else {
            lock->vinfo_kind = LHH_VINFO_DRAWER;
        }
        lhh_lock_set_key(lock);
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
        lhh_lock_set_key(lock);
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
    lhh_lock_set_key(lock);
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
    if (src->type == LHH_LOCK_VINFO) {
        return lhh_lock_alloc(gd, LHH_LOCK_VINFO, NULL,
            (src->vinfo_kind == LHH_VINFO_DISK) ? "DISK" : NULL,
            ZERO, NULL, src->virt_path, src->access, err);
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
        /* One component: bare volume/assign name (RAM, VolumeName, ...).
         * Names with ':' are invalid under LHA: (LHA:VolumeName: fails). */
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

    if (lock->type == LHH_LOCK_VINFO) {
        static char base[LHH_PATH_LEN];
        static UBYTE bstr[LHH_PATH_LEN + 1];
        LONG ln;
        LONG i;

        /* Disk.info / Volume.info sit at LHA: root. */
        if (lock->vinfo_kind == LHH_VINFO_DISK) {
            return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
                NULL, access, err);
        }
        if (!lhh_strip_info_suffix(lock->virt_path, base, LHH_PATH_LEN)) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        ln = lhh_cstr_len(base);
        if (ln > 255) {
            ln = 255;
        }
        bstr[0] = (UBYTE)ln;
        for (i = 0; i < ln; i++) {
            bstr[i + 1] = (UBYTE)base[i];
        }
        return lhh_lock_resolve(gd, NULL, (STRPTR)bstr, access, err);
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
        static UBYTE bstr[LHH_PATH_LEN + 1];
        LONG ln;
        LONG i;

        /*
         * OpenWorkbenchObject(LHA:work/file.lha) locks the archive (drawer),
         * then ParentDir -> LHA:work.  WB never opens file.lha.info itself
         * (boemann).  Parent must stay on our handler (fl_Task), not a bare
         * host lock, or GetDiskObject would see the host project .info.
         */
        if (!lhh_virt_parent_path(lock->virt_path, pvirt, LHH_PATH_LEN)) {
            DB1("parent ARCHIVE %s -> ROOT\n", lock->virt_path);
            return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
                NULL, access, err);
        }
        ln = lhh_cstr_len(pvirt);
        if (ln > 255) {
            ln = 255;
        }
        bstr[0] = (UBYTE)ln;
        for (i = 0; i < ln; i++) {
            bstr[i + 1] = (UBYTE)pvirt[i];
        }
        DB2("parent ARCHIVE %s -> %s\n", lock->virt_path, pvirt);
        return lhh_lock_resolve(gd, NULL, (STRPTR)bstr, access, err);
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

    /*
     * *.info handling:
     *  Inside ARCHIVE/ENTRY: real catalog .info first; if missing, magic
     *  VINFO so ExNext-synthesized placeholders (Show All / default icons)
     *  still Open for GetDiskObject.
     *  Outside: real host .info, else Disk.info WBDISK, else drawer/.lha
     *  VINFO when the companion is a drawer or archive.
     */
    if (lhh_name_ends_info(comp) || lhh_is_disk_info_name(comp)) {
        static char vinfo[LHH_PATH_LEN];
        static char realinfo[LHH_PATH_LEN];
        static char basevirt[LHH_PATH_LEN];
        static char basereal[LHH_PATH_LEN];
        static char entinfo[LHH_PATH_LEN];
        BPTR ilock;
        BPTR lh;
        struct LhHLock *real_lock;
        struct FileInfoBlock *fib;
        int magic_drawer;

        if (lhh_cstr_eq_i(comp, ".info") && parent != NULL
            && parent->virt_path[0]) {
            lhh_cstr_copy(vinfo, parent->virt_path, LHH_PATH_LEN);
            {
                LONG blen;

                blen = lhh_cstr_len(vinfo);
                if (blen + 5 < LHH_PATH_LEN) {
                    vinfo[blen] = '.';
                    vinfo[blen + 1] = 'i';
                    vinfo[blen + 2] = 'n';
                    vinfo[blen + 3] = 'f';
                    vinfo[blen + 4] = 'o';
                    vinfo[blen + 5] = '\0';
                }
            }
        } else if (parent != NULL && parent->virt_path[0]
            && !lhh_cstr_has(comp, '/')) {
            lhh_virt_append_name(vinfo, LHH_PATH_LEN, parent->virt_path,
                comp);
        } else {
            lhh_cstr_copy(vinfo, comp, LHH_PATH_LEN);
        }

        /* Archive: prefer a real catalog .info entry when present. */
        if (parent != NULL
            && (parent->type == LHH_LOCK_ARCHIVE
                || parent->type == LHH_LOCK_ENTRY)
            && parent->arc != NULL && parent->arc->archive != NULL) {
            if (parent->type == LHH_LOCK_ENTRY && parent->entry[0]
                && !lhh_cstr_has(comp, '/')) {
                lhh_virt_append_name(entinfo, LHH_PATH_LEN, parent->entry,
                    comp);
            } else {
                lhh_cstr_copy(entinfo, comp, LHH_PATH_LEN);
            }
            lh = LhLock(parent->arc->archive, (STRPTR)entinfo);
            if (lh != ZERO) {
                LhUnLock(lh);
                /* Fall through to normal ARCHIVE/ENTRY resolve below. */
            } else {
                /* Synthesized placeholder from ExNext - default drawer icon. */
                return lhh_lock_alloc(gd, LHH_LOCK_VINFO, NULL, NULL, ZERO,
                    NULL, vinfo, access, err);
            }
        } else {
        /* Prefer a real host .info when one exists. */
        if (lhh_virt_to_real(gd, vinfo, realinfo, LHH_PATH_LEN)
            && realinfo[0] != '\0'
            && !lhh_is_self_real(gd, realinfo)) {
            ilock = lhh_host_lock((STRPTR)realinfo, access);
            if (ilock != ZERO) {
                return lhh_lock_alloc(gd, LHH_LOCK_REAL, NULL, NULL, ilock,
                    realinfo, vinfo, access, err);
            }
        }

        /* Volume Disk.info / Volume.info -> WBDISK default. */
        if (lhh_is_disk_info_name(comp) || lhh_is_disk_info_name(vinfo)) {
            return lhh_lock_alloc(gd, LHH_LOCK_VINFO, NULL, "DISK", ZERO, NULL,
                vinfo, access, err);
        }

        /*
         * Magic drawer icon only if the base object is a host drawer or
         * a .lha presented as ARCHIVE.
         */
        magic_drawer = 0;
        if (lhh_strip_info_suffix(vinfo, basevirt, LHH_PATH_LEN)
            && lhh_virt_to_real(gd, basevirt, basereal, LHH_PATH_LEN)
            && basereal[0] != '\0') {
            real_lock = lhh_lock_real_path(gd, basereal, basevirt, SHARED_LOCK,
                err);
            if (real_lock != NULL) {
                if (real_lock->type == LHH_LOCK_ARCHIVE) {
                    magic_drawer = 1;
                } else if (real_lock->type == LHH_LOCK_REAL
                    && real_lock->real_lock != ZERO) {
                    fib = (struct FileInfoBlock *)AllocMem(
                        sizeof(struct FileInfoBlock),
                        MEMF_PUBLIC | MEMF_CLEAR);
                    if (fib != NULL) {
                        if (lhh_host_examine(real_lock->real_lock, fib)
                            && fib->fib_DirEntryType > 0) {
                            magic_drawer = 1;
                        }
                        FreeMem(fib, sizeof(struct FileInfoBlock));
                    }
                }
                lhh_lock_free(gd, real_lock);
            }
            if (err) {
                *err = 0;
            }
        }
        if (magic_drawer) {
            return lhh_lock_alloc(gd, LHH_LOCK_VINFO, NULL, NULL, ZERO, NULL,
                vinfo, access, err);
        }
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
        }
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
     * Nested walk: locate relative to the parent's host lock.  Fall back
     * to a full-path host lock if the relative packet fails.
     */
    if (parent != NULL && parent->type == LHH_LOCK_REAL
        && parent->real_lock != ZERO) {
        rlock = lhh_host_lock_from(parent->real_lock, (STRPTR)comp, access);
        if (rlock == ZERO) {
            DB3("host_lock_from fail comp=%s real=%s err=%ld\n",
                comp, real, (LONG)IoErr());
            rlock = lhh_host_lock((STRPTR)real, access);
        }
        if (rlock == ZERO) {
            DB2("host_lock fail real=%s err=%ld\n", real, (LONG)IoErr());
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
    /*
     * Strip trailing '/' so "lhasa/" matches "lhasa".  Leave pure "/"
     * chains alone - they mean parent / grandparent (cd /, cd //).
     */
    if (!lhh_is_parent_chain(name)) {
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
    } else if (name[0] == ':' && name[1] == '\0') {
        /*
         * Relocate ":" under any LHA: lock = volume root (WB MakeDiskLock).
         */
        return lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL, ZERO, NULL,
            NULL, access, err);
    } else if (name[0] == '\0') {
        return lhh_lock_dup(gd, parent, err);
    }

    /*
     * Amiga relative parent: Lock(dir,"/") / "cd /", and "//" for two up.
     * Must run before ARCHIVE/ENTRY name handling, which would otherwise
     * treat "/" as an entry path or reject ENTRY-relative locates.
     */
    if (parent != NULL && parent->type != LHH_LOCK_ROOT
        && lhh_is_parent_chain(name)) {
        struct LhHLock *cur;
        struct LhHLock *next;
        const char *rest;

        cur = parent;
        rest = name;
        while (rest[0] != '\0') {
            if (!lhh_path_next(&rest, comp, LHH_PATH_LEN)
                || !lhh_is_parent_name(comp)) {
                if (cur != parent) {
                    lhh_lock_free(gd, cur);
                }
                if (err) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
                return NULL;
            }
            next = lhh_lock_parent(gd, cur, err);
            if (next == NULL) {
                if (cur != parent) {
                    lhh_lock_free(gd, cur);
                }
                return NULL;
            }
            if (cur != parent) {
                lhh_lock_free(gd, cur);
            }
            cur = next;
        }
        return cur;
    }

    if (parent != NULL && parent->type == LHH_LOCK_ENTRY) {
        static char newentry[LHH_PATH_LEN];
        static char ventry[LHH_PATH_LEN];

        /*
         * Relative locate under an archive directory: "subdir" or
         * "subdir/file".  Parent steps were handled above.
         */
        if (!name[0] || lhh_cstr_has(name, ':')) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        arc = parent->arc;
        if (arc == NULL) {
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return NULL;
        }
        lhh_virt_append_name(newentry, LHH_PATH_LEN, parent->entry, name);
        lhh_virt_append_name(ventry, LHH_PATH_LEN, parent->virt_path, name);
        arc->refcount++;
        return lhh_lock_alloc(gd, LHH_LOCK_ENTRY, arc, newentry, ZERO,
            parent->real_path, ventry, access, err);
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
     * Free intermediate locks so nested paths do not leak host locks.
     */
    lock = parent;
    rest = name;
    while (rest != NULL && rest[0] != '\0') {
        struct LhHLock *prev;

        if (lock != NULL && lock->type == LHH_LOCK_ARCHIVE) {
            break;
        }
        if (!lhh_path_next(&rest, comp, LHH_PATH_LEN)) {
            break;
        }
        prev = lock;
        lock = lhh_lock_resolve_one(gd, prev, comp, access, err);
        if (lock == NULL) {
            if (prev != NULL && prev != parent) {
                lhh_lock_free(gd, prev);
            }
            return NULL;
        }
        if (prev != NULL && prev != parent && prev != lock) {
            lhh_lock_free(gd, prev);
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
    hh->mem_data = NULL;
    hh->mem_len = 0;
    hh->mem_pos = 0;
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
    static UBYTE namebstr[LHH_PATH_LEN + 1];
    LONG access;
    LONG ln;
    LONG i;

    lhh_bstr_to_cstr(name_bstr, name, LHH_PATH_LEN);
    /*
     * Open("LHA:Work/t/foo.lha.info") may arrive with the device prefix
     * still in the name (same as locate).
     */
    lhh_strip_self_prefix(gd, name);

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
        /* Rebuild BSTR after possible LHA: strip for resolve. */
        ln = lhh_cstr_len(name);
        if (ln > 255) {
            ln = 255;
        }
        namebstr[0] = (UBYTE)ln;
        for (i = 0; i < ln; i++) {
            namebstr[i + 1] = (UBYTE)name[i];
        }
        lock = lhh_lock_resolve(gd, parent, (STRPTR)namebstr, access, err);
    }

    /*
     * Embedded icon bytes only for VINFO locks (placeholders outside
     * archives).  Never inject icons for *.info entries inside a .lha.
     */
    if (mode == MODE_OLDFILE && lock != NULL && lock->type == LHH_LOCK_VINFO) {
        int is_disk;

        hh = lhh_handle_alloc(gd, err);
        if (hh == NULL) {
            if (lock != parent) {
                lhh_lock_free(gd, lock);
            }
            return NULL;
        }
        is_disk = lhh_want_disk_icon(lock, name);
        lhh_set_icon_mem(hh, is_disk ? LHH_VINFO_DISK : LHH_VINFO_DRAWER);
        if (lock != parent) {
            lhh_lock_free(gd, lock);
        }
        gd->gd_UsageCnt++;
        DB1(is_disk ? "disk icon open name=%s\n"
            : "drawer icon open name=%s\n",
            name[0] ? name : "(lock)");
        return hh;
    }

    /* New archive entry under ARCHIVE or ENTRY (nested) lock. */
    if (lock == NULL && (mode == MODE_NEWFILE || mode == MODE_READWRITE)
        && parent != NULL
        && (parent->type == LHH_LOCK_ARCHIVE
            || parent->type == LHH_LOCK_ENTRY) && name[0]) {
        static char newentry[LHH_PATH_LEN];

        lock = lhh_lock_dup(gd, parent, err);
        if (lock != NULL) {
            hh = lhh_handle_alloc(gd, err);
            if (hh == NULL) {
                lhh_lock_free(gd, lock);
                return NULL;
            }
            hh->lock = lock;
            hh->writing = 1;
            if (parent->type == LHH_LOCK_ENTRY && parent->entry[0]) {
                lhh_virt_append_name(newentry, LHH_PATH_LEN, parent->entry,
                    name);
                lhh_cstr_copy(hh->entry, newentry, LHH_PATH_LEN);
            } else {
                lhh_cstr_copy(hh->entry, name, LHH_PATH_LEN);
            }
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
        struct FileInfoBlock *efib;

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
        /*
         * Directories must not be Open()'d as files - copy uses
         * Lock/ExNext/COPY_DIR_FH instead.  Opening a dir FH and then
         * failing COPY_DIR_FH caused unexpected-packet crashes.
         */
        efib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (efib != NULL && lock->lh_lock != ZERO
            && LhExamine(lock->lh_lock, efib)
            && efib->fib_DirEntryType > 0) {
            FreeMem(efib, sizeof(struct FileInfoBlock));
            lhh_lock_free(gd, lock);
            FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
            if (err) {
                *err = ERROR_OBJECT_WRONG_TYPE;
            }
            return NULL;
        }
        if (efib != NULL) {
            FreeMem(efib, sizeof(struct FileInfoBlock));
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

/*
 * ACTION_FH_FROM_LOCK: open a file from an existing lock.  On success the
 * lock is stolen (caller must not UnLock).  Icon.library V44+ uses this
 * for .info after Lock+Examine; without it OpenWorkbench gets wrong type.
 */
struct LhHHandle *lhh_handle_from_lock(GD gd, struct LhHLock *lock,
    LONG mode, LONG *err)
{
    struct LhHHandle *hh;
    BPTR fh;

    if (lock == NULL) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return NULL;
    }

    if (mode == MODE_OLDFILE && lock->type == LHH_LOCK_VINFO) {
        int is_disk;

        hh = lhh_handle_alloc(gd, err);
        if (hh == NULL) {
            return NULL;
        }
        is_disk = lhh_want_disk_icon(lock, NULL);
        lhh_set_icon_mem(hh, is_disk ? LHH_VINFO_DISK : LHH_VINFO_DRAWER);
        /* Steal: consume the VINFO lock now that the mem file is open. */
        lhh_lock_free(gd, lock);
        gd->gd_UsageCnt++;
        DB(is_disk ? "disk icon open from lock\n"
            : "drawer icon open from lock\n");
        return hh;
    }

    hh = lhh_handle_alloc(gd, err);
    if (hh == NULL) {
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
            hh->lock = NULL;
            FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
            return NULL;
        }
        hh->real_fh = fh;
        gd->gd_UsageCnt++;
        return hh;
    }

    if (lock->type == LHH_LOCK_ENTRY) {
        struct FileInfoBlock *efib;

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
        efib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (efib != NULL && lock->lh_lock != ZERO
            && LhExamine(lock->lh_lock, efib)
            && efib->fib_DirEntryType > 0) {
            FreeMem(efib, sizeof(struct FileInfoBlock));
            hh->lock = NULL;
            FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
            if (err) {
                *err = ERROR_OBJECT_WRONG_TYPE;
            }
            return NULL;
        }
        if (efib != NULL) {
            FreeMem(efib, sizeof(struct FileInfoBlock));
        }
        fh = LhOpenFromLock(lock->lh_lock, mode);
        if (fh == ZERO) {
            if (err) {
                *err = LhErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            hh->lock = NULL;
            FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
            return NULL;
        }
        hh->lh_fh = fh;
        gd->gd_UsageCnt++;
        return hh;
    }

    /* ROOT/ARCHIVE/etc are not openable as files. */
    hh->lock = NULL;
    FreePooled(gd->gd_Pool, hh, sizeof(struct LhHHandle));
    if (err) {
        *err = ERROR_OBJECT_WRONG_TYPE;
    }
    return NULL;
}

LONG lhh_handle_read(GD gd, struct LhHHandle *hh, APTR buf, LONG len)
{
    ULONG left;
    ULONG n;

    (void)gd;

    if (hh == NULL || hh->writing || !buf || len <= 0) {
        return -1;
    }
    if (hh->mem_data != NULL) {
        if (hh->mem_pos >= hh->mem_len) {
            return 0;
        }
        left = hh->mem_len - hh->mem_pos;
        n = (ULONG)len;
        if (n > left) {
            n = left;
        }
        CopyMem((APTR)(hh->mem_data + hh->mem_pos), buf, n);
        hh->mem_pos += n;
        return (LONG)n;
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
    LONG newpos;

    (void)gd;

    if (hh == NULL) {
        return -1;
    }
    if (hh->mem_data != NULL) {
        if (mode == OFFSET_BEGINNING) {
            newpos = pos;
        } else if (mode == OFFSET_CURRENT) {
            newpos = (LONG)hh->mem_pos + pos;
        } else if (mode == OFFSET_END) {
            newpos = (LONG)hh->mem_len + pos;
        } else {
            return -1;
        }
        if (newpos < 0 || (ULONG)newpos > hh->mem_len) {
            return -1;
        }
        hh->mem_pos = (ULONG)newpos;
        return newpos;
    }
    if (hh->real_fh != ZERO) {
        return lhh_host_seek(hh->real_fh, pos, mode);
    }
    if (hh->writing) {
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

/*
 * Rename one archive entry (file or -lhd- dir) via LhReadData / Delete / Add.
 * Empty directories use AddEntry + LHADD_Directory after delete.
 */
static LONG lhh_arc_rename_one(GD gd, struct LhHArc *ha, const char *oldn,
    const char *newn, LONG *err)
{
    APTR data;
    LONG len;
    LONG ok;
    struct LhArchive *warc;
    struct TagItem tags[2];
    if (ha == NULL || ha->archive == NULL || oldn == NULL || newn == NULL) {
        if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
        return DOSFALSE;
    }
    if (lhh_cstr_eq_i(oldn, newn)) {
        return DOSTRUE;
    }

    data = NULL;
    len = LhReadData(ha->archive, (STRPTR)oldn, &data);
    if (len < 0) {
        /* Likely an empty -lhd- directory (or missing payload). */
        len = 0;
        data = NULL;
    }

    if (!LhDeleteFile(ha->archive, (STRPTR)oldn)) {
        if (data != NULL) {
            FreeMem(data, (ULONG)len);
        }
        if (err) {
            *err = LhErr();
            if (*err == 0) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
        }
        return DOSFALSE;
    }

    warc = LhOpenArchive((STRPTR)ha->real_path, LHARC_MODE_APPEND);
    if (warc == NULL) {
        if (data != NULL) {
            FreeMem(data, (ULONG)len);
        }
        if (err) {
            *err = LhErr();
            if (*err == 0) {
                *err = ERROR_WRITE_PROTECTED;
            }
        }
        return DOSFALSE;
    }

    if (len == 0 && data == NULL) {
        tags[0].ti_Tag = LHADD_Directory;
        tags[0].ti_Data = 1;
        tags[1].ti_Tag = TAG_DONE;
        ok = LhAddEntryTagList(warc, (STRPTR)newn, NULL, 0, tags);
    } else {
        ok = LhAddEntry(warc, (STRPTR)newn, data, len);
    }
    LhCloseArchive(warc);
    if (data != NULL) {
        FreeMem(data, (ULONG)len);
    }
    if (!ok) {
        if (err) {
            *err = LhErr();
            if (*err == 0) {
                *err = ERROR_WRITE_PROTECTED;
            }
        }
        return DOSFALSE;
    }
    return DOSTRUE;
}

/*
 * True if catalog name en is from_entry or a child under from_entry/.
 */
static int lhh_rename_prefix_match(const char *en, const char *from_entry,
    LONG oldlen)
{
    LONG i;
    char a;
    char b;

    if (en == NULL || from_entry == NULL) {
        return 0;
    }
    if (lhh_cstr_eq_i(en, from_entry)) {
        return 1;
    }
    if ((LONG)lhh_cstr_len(en) <= oldlen || en[oldlen] != '/') {
        return 0;
    }
    for (i = 0; i < oldlen; i++) {
        a = en[i];
        b = from_entry[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static void lhh_rename_rebuild(const char *en, const char *from_entry,
    const char *to_entry, LONG oldlen, char *out, LONG outmax)
{
    LONG tlen;

    if (lhh_cstr_eq_i(en, from_entry)) {
        lhh_cstr_copy(out, to_entry, outmax);
        return;
    }
    tlen = lhh_cstr_len(to_entry);
    if (tlen + (lhh_cstr_len(en) - oldlen) + 1 >= outmax) {
        out[0] = '\0';
        return;
    }
    lhh_cstr_copy(out, to_entry, outmax);
    out[tlen] = '/';
    lhh_cstr_copy(out + tlen + 1, en + oldlen + 1, outmax - tlen - 1);
}

/*
 * Collect rename pairs via nested ExNext (read-only).  Apply after the walk
 * so locks/catalog stay valid while listing.
 */
struct lhh_rn_item {
    struct lhh_rn_item *next;
    char oldn[LHH_PATH_LEN];
    char newn[LHH_PATH_LEN];
};

static void lhh_rn_free(GD gd, struct lhh_rn_item *head)
{
    struct lhh_rn_item *n;

    while (head != NULL) {
        n = head->next;
        FreeMem(head, sizeof(struct lhh_rn_item));
        head = n;
    }
}

static LONG lhh_rename_collect(GD gd, struct LhHArc *ha, const char *dir,
    const char *from_entry, const char *to_entry, LONG oldlen,
    struct lhh_rn_item **head, LONG *err)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG dlen;
    LONG nlen;
    char childpath[LHH_PATH_LEN];
    char rebuilt[LHH_PATH_LEN];
    struct lhh_rn_item *item;

    lock = LhLock(ha->archive, dir ? (STRPTR)dir : (STRPTR)"");
    if (lock == ZERO) {
        return DOSFALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhUnLock(lock);
        if (err) {
            *err = ERROR_NO_FREE_STORE;
        }
        return DOSFALSE;
    }
    if (!LhExamine(lock, fib)) {
        FreeMem(fib, sizeof(struct FileInfoBlock));
        LhUnLock(lock);
        return DOSTRUE;
    }
    ok = LhExNext(lock, fib);
    while (ok) {
        dlen = 0;
        if (dir != NULL && dir[0] != '\0') {
            while (dir[dlen] != '\0' && dlen < LHH_PATH_LEN - 2) {
                childpath[dlen] = dir[dlen];
                dlen++;
            }
            childpath[dlen++] = '/';
        }
        nlen = 0;
        while (fib->fib_FileName[nlen] != '\0'
            && dlen + nlen < LHH_PATH_LEN - 1) {
            childpath[dlen + nlen] = fib->fib_FileName[nlen];
            nlen++;
        }
        childpath[dlen + nlen] = '\0';

        if (fib->fib_DirEntryType > 0) {
            lhh_rename_collect(gd, ha, childpath, from_entry, to_entry, oldlen,
                head, err);
            if (lhh_cstr_eq_i(childpath, from_entry)) {
                lhh_rename_rebuild(childpath, from_entry, to_entry, oldlen,
                    rebuilt, LHH_PATH_LEN);
                if (rebuilt[0]) {
                    item = (struct lhh_rn_item *)AllocMem(
                        sizeof(struct lhh_rn_item), MEMF_PUBLIC | MEMF_CLEAR);
                    if (item == NULL) {
                        if (err) {
                            *err = ERROR_NO_FREE_STORE;
                        }
                        FreeMem(fib, sizeof(struct FileInfoBlock));
                        LhUnLock(lock);
                        return DOSFALSE;
                    }
                    lhh_cstr_copy(item->oldn, childpath, LHH_PATH_LEN);
                    lhh_cstr_copy(item->newn, rebuilt, LHH_PATH_LEN);
                    item->next = *head;
                    *head = item;
                }
            }
        } else if (lhh_rename_prefix_match(childpath, from_entry, oldlen)) {
            lhh_rename_rebuild(childpath, from_entry, to_entry, oldlen,
                rebuilt, LHH_PATH_LEN);
            if (rebuilt[0]) {
                item = (struct lhh_rn_item *)AllocMem(
                    sizeof(struct lhh_rn_item), MEMF_PUBLIC | MEMF_CLEAR);
                if (item == NULL) {
                    if (err) {
                        *err = ERROR_NO_FREE_STORE;
                    }
                    FreeMem(fib, sizeof(struct FileInfoBlock));
                    LhUnLock(lock);
                    return DOSFALSE;
                }
                lhh_cstr_copy(item->oldn, childpath, LHH_PATH_LEN);
                lhh_cstr_copy(item->newn, rebuilt, LHH_PATH_LEN);
                item->next = *head;
                *head = item;
            }
        }
        ok = LhExNext(lock, fib);
    }
    FreeMem(fib, sizeof(struct FileInfoBlock));
    LhUnLock(lock);
    return DOSTRUE;
}

LONG lhh_rename_name(GD gd, struct LhHLock *from_parent, STRPTR from_bstr,
    struct LhHLock *to_parent, STRPTR to_bstr, LONG *err)
{
    struct LhHLock *from_lock;
    struct LhHLock *to_exist;
    struct FileInfoBlock *fib;
    struct lhh_rn_item *head;
    struct lhh_rn_item *it;
    static char from_entry[LHH_PATH_LEN];
    static char to_entry[LHH_PATH_LEN];
    static char dest_name[LHH_PATH_LEN];
    static char newpath[LHH_PATH_LEN];
    LONG ok;
    LONG oldlen;
    LONG nrenamed;

#ifndef ERROR_RENAME_ACROSS_DEVICES
#define ERROR_RENAME_ACROSS_DEVICES 215
#endif

    from_lock = lhh_lock_resolve(gd, from_parent, from_bstr, SHARED_LOCK, err);
    if (from_lock == NULL) {
        return DOSFALSE;
    }

    to_exist = lhh_lock_resolve(gd, to_parent, to_bstr, SHARED_LOCK, err);
    if (to_exist != NULL) {
        lhh_lock_free(gd, to_exist);
        lhh_lock_free(gd, from_lock);
        if (err) {
            *err = ERROR_OBJECT_EXISTS;
        }
        return DOSFALSE;
    }
    if (err) {
        *err = 0;
    }

    /* ---- rename inside an archive ---- */
    if (from_lock->type == LHH_LOCK_ENTRY && from_lock->arc != NULL) {
        lhh_cstr_copy(from_entry, from_lock->entry, LHH_PATH_LEN);
        lhh_bstr_to_cstr(to_bstr, dest_name, LHH_PATH_LEN);

        if (to_parent != NULL && to_parent->type == LHH_LOCK_ENTRY
            && to_parent->arc == from_lock->arc) {
            lhh_virt_append_name(to_entry, LHH_PATH_LEN, to_parent->entry,
                dest_name);
        } else if (to_parent != NULL && to_parent->type == LHH_LOCK_ARCHIVE
            && to_parent->arc == from_lock->arc) {
            lhh_cstr_copy(to_entry, dest_name, LHH_PATH_LEN);
        } else {
            lhh_lock_free(gd, from_lock);
            if (err) {
                *err = ERROR_RENAME_ACROSS_DEVICES;
            }
            return DOSFALSE;
        }

        /* Single file: read / delete / add under new name. */
        fib = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (fib != NULL && from_lock->lh_lock != ZERO
            && LhExamine(from_lock->lh_lock, fib)
            && fib->fib_DirEntryType < 0) {
            FreeMem(fib, sizeof(struct FileInfoBlock));
            ok = lhh_arc_rename_one(gd, from_lock->arc, from_entry, to_entry,
                err);
            if (ok) {
                lhh_arc_refresh(gd, from_lock->arc, err);
            }
            lhh_lock_free(gd, from_lock);
            return ok;
        }
        if (fib != NULL) {
            FreeMem(fib, sizeof(struct FileInfoBlock));
        }

        /*
         * Directory (or synth dir): collect nested catalog paths, then
         * rename each.  Collect is read-only so ExNext locks stay valid.
         */
        oldlen = lhh_cstr_len(from_entry);
        head = NULL;
        if (!lhh_rename_collect(gd, from_lock->arc, "", from_entry, to_entry,
            oldlen, &head, err)) {
            lhh_rn_free(gd, head);
            lhh_lock_free(gd, from_lock);
            return DOSFALSE;
        }
        nrenamed = 0;
        for (it = head; it != NULL; it = it->next) {
            if (lhh_arc_rename_one(gd, from_lock->arc, it->oldn, it->newn,
                err)) {
                nrenamed++;
            }
        }
        lhh_rn_free(gd, head);
        if (nrenamed > 0) {
            lhh_arc_refresh(gd, from_lock->arc, err);
            lhh_lock_free(gd, from_lock);
            return DOSTRUE;
        }
        lhh_lock_free(gd, from_lock);
        if (err && *err == 0) {
            *err = ERROR_OBJECT_WRONG_TYPE;
        }
        return DOSFALSE;
    }

    /* ---- rename on host through LHA: REAL / ARCHIVE file ---- */
    if (from_lock->type == LHH_LOCK_REAL
        || from_lock->type == LHH_LOCK_ARCHIVE) {
        lhh_cstr_copy(from_entry, from_lock->real_path, LHH_PATH_LEN);
        lhh_bstr_to_cstr(to_bstr, dest_name, LHH_PATH_LEN);
        if (to_parent != NULL && to_parent->type == LHH_LOCK_REAL
            && to_parent->real_path[0]) {
            lhh_real_append_name(newpath, LHH_PATH_LEN, to_parent->real_path,
                dest_name);
        } else if (!lhh_virt_to_real(gd, dest_name, newpath, LHH_PATH_LEN)) {
            lhh_lock_free(gd, from_lock);
            if (err) {
                *err = ERROR_OBJECT_NOT_FOUND;
            }
            return DOSFALSE;
        }
        lhh_lock_free(gd, from_lock);
        ok = lhh_host_rename((STRPTR)from_entry, (STRPTR)newpath);
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

    lhh_lock_free(gd, from_lock);
    if (err) {
        *err = ERROR_OBJECT_WRONG_TYPE;
    }
    return DOSFALSE;
}

/*
 * CreateDir under an archive: add an -lhd- entry and return a lock on it.
 * Parent must be ARCHIVE or ENTRY.  Host CreateDir uses dos.library.
 */
struct LhHLock *lhh_create_dir_name(GD gd, struct LhHLock *parent,
    STRPTR name_bstr, LONG *err)
{
    struct LhHLock *exist;
    struct LhArchive *warc;
    struct TagItem tags[2];
    static char name[LHH_PATH_LEN];
    static char entry[LHH_PATH_LEN];
    static char ventry[LHH_PATH_LEN];
    static char realpath[LHH_PATH_LEN];
    LONG ok;
    struct LhHArc *ha;
    BPTR dlock;

    lhh_bstr_to_cstr(name_bstr, name, LHH_PATH_LEN);
    if (name[0] == '\0') {
        if (err) {
            *err = ERROR_INVALID_COMPONENT_NAME;
        }
        return NULL;
    }

    exist = lhh_lock_resolve(gd, parent, name_bstr, SHARED_LOCK, err);
    if (exist != NULL) {
        lhh_lock_free(gd, exist);
        if (err) {
            *err = ERROR_OBJECT_EXISTS;
        }
        return NULL;
    }
    if (err) {
        *err = 0;
    }

    if (parent != NULL && (parent->type == LHH_LOCK_ARCHIVE
        || parent->type == LHH_LOCK_ENTRY) && parent->arc != NULL) {
        ha = parent->arc;
        if (parent->type == LHH_LOCK_ENTRY && parent->entry[0]) {
            lhh_virt_append_name(entry, LHH_PATH_LEN, parent->entry, name);
        } else {
            lhh_cstr_copy(entry, name, LHH_PATH_LEN);
        }
        lhh_virt_append_name(ventry, LHH_PATH_LEN, parent->virt_path, name);
        lhh_cstr_copy(realpath, ha->real_path, LHH_PATH_LEN);

        warc = LhOpenArchive((STRPTR)realpath, LHARC_MODE_APPEND);
        if (warc == NULL) {
            if (err) {
                *err = LhErr();
                if (*err == 0) {
                    *err = ERROR_WRITE_PROTECTED;
                }
            }
            return NULL;
        }
        tags[0].ti_Tag = LHADD_Directory;
        tags[0].ti_Data = 1;
        tags[1].ti_Tag = TAG_DONE;
        ok = LhAddEntryTagList(warc, (STRPTR)entry, NULL, 0, tags);
        LhCloseArchive(warc);
        if (!ok) {
            if (err) {
                *err = LhErr();
                if (*err == 0) {
                    *err = ERROR_WRITE_PROTECTED;
                }
            }
            return NULL;
        }
        if (!lhh_arc_refresh(gd, ha, err)) {
            return NULL;
        }
        ha->refcount++;
        return lhh_lock_alloc(gd, LHH_LOCK_ENTRY, ha, entry, ZERO, realpath,
            ventry, SHARED_LOCK, err);
    }

    /* Host drawer next to archives under LHA: REAL. */
    if (parent != NULL && parent->type == LHH_LOCK_REAL
        && parent->real_path[0]) {
        lhh_real_append_name(realpath, LHH_PATH_LEN, parent->real_path, name);
        dlock = CreateDir((STRPTR)realpath);
        if (dlock == ZERO) {
            if (err) {
                *err = IoErr();
                if (*err == 0) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            }
            return NULL;
        }
        UnLock(dlock);
        return lhh_lock_resolve(gd, parent, name_bstr, SHARED_LOCK, err);
    }

    if (err) {
        *err = ERROR_OBJECT_WRONG_TYPE;
    }
    return NULL;
}
