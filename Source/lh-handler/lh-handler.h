/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh-handler.h - LHA: overlay filesystem handler (lh.library).
 *
 * LHA: root lists mounted DosList volumes only (not assigns).  Non-archive paths
 * passthrough to the real filesystem via host packet, never dos.library calls.
 * Valid LHA archives are presented as directories through lh.library.
 */

#ifndef LH_HANDLER_H
#define LH_HANDLER_H

#define __USE_SYSBASE 42

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/exall.h>

#include <proto/exec.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include <libraries/lhlib.h>
#include <proto/lh.h>

#include "lh-handler_rev.h"

#include "lhh_debug.h"

#ifdef __SASC
#define RegCall      __asm
#define GetA4        __saveds
#define REGA0        register __a0
#define REGD0        register __d0
#else
#define RegCall
#define GetA4
#define REGA0
#define REGD0
#endif

#ifndef ZERO
#define ZERO 0
#endif

#define LHH_PATH_LEN   512
#define LHH_PUDDLE     4096
#define LHH_LOCK_MAGIC 0x4C48414BL  /* 'LHAL' */

#define LHH_LOCK_ROOT     0
#define LHH_LOCK_REAL     1  /* passthrough: real dos Lock on another volume */
#define LHH_LOCK_ARCHIVE  2
#define LHH_LOCK_ENTRY    3
#define LHH_LOCK_VINFO    4  /* virtual .info (WBDISK at root; else WBDRAWER) */

struct LhHArc {
    struct MinNode node;
    char real_path[LHH_PATH_LEN];
    struct LhArchive *archive;
    LONG refcount;
};

struct LhHLock {
    struct FileLock fl;
    ULONG magic;
    ULONG type;
    struct LhHArc *arc;
    BPTR lh_lock;                 /* lh.library lock (ARCHIVE/ENTRY) */
    BPTR real_lock;               /* dos.library lock (REAL) */
    char entry[LHH_PATH_LEN];     /* archive entry name */
    char real_path[LHH_PATH_LEN]; /* real Amiga path (REAL/ARCHIVE) */
    char virt_path[LHH_PATH_LEN]; /* path under LHA: (no LHA: prefix) */
    LONG access;
    /* Kludge: next ExNext emits this leaf as a virtual .info file. */
    char pending_info[108];
    LONG pending_diskkey;         /* preserve ExNext cursor across .info */
    struct DateStamp pending_date; /* date for the synthetic .info */
    UBYTE vinfo_kind;             /* LHH_VINFO_DRAWER or LHH_VINFO_DISK */
};

struct LhHHandle {
    struct LhHLock *lock;
    BPTR lh_fh;                   /* archive entry handle */
    BPTR real_fh;                 /* passthrough file handle */
    ULONG writing;
    UBYTE *wbuf;
    ULONG wlen;
    ULONG wcap;
    char entry[LHH_PATH_LEN];
    /* Virtual *.lha.info -> embedded Drawer.info (read-only). */
    const UBYTE *mem_data;
    ULONG mem_len;
    ULONG mem_pos;
};

struct GlobalData {
    struct Library *gd_SysBase;
    struct Library *gd_DOSBase;
    struct Library *gd_UtilityBase;
    struct Library *gd_LhBase;
    APTR gd_Pool;
    struct MsgPort *gd_Port;
    struct DosList *gd_DosList;   /* DLT_DEVICE from mount */
    struct DosList *gd_Volume;    /* DLT_VOLUME we publish for WB */
    struct Process *gd_We;
    ULONG gd_UsageCnt;
    ULONG gd_LockCnt;
    ULONG gd_VolumeAdded;
    ULONG gd_VolPending;
    struct MinList gd_Arcs;
};

typedef struct GlobalData *GD;

/* Set in lhh_main before any host FS call (lh_pkt.c uses this). */
extern GD LhhGD;
extern struct Library *LhBase;

#ifdef __SASC
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#define UtilityBase gd->gd_UtilityBase
#endif

/* lh_path.c */
void lhh_bstr_to_cstr(STRPTR bstr, STRPTR out, LONG outlen);
void lhh_cstr_copy(STRPTR dst, const char *src, LONG dstlen);
LONG lhh_cstr_len(const char *s);
int lhh_cstr_eq(const char *a, const char *b);
int lhh_cstr_eq_i(const char *a, const char *b);
void lhh_path_join(STRPTR out, LONG outlen, const char *base, const char *name);
int lhh_is_parent_name(const char *name);
int lhh_is_parent_chain(const char *name);
int lhh_is_lha_info_name(const char *name);
int lhh_strip_info_suffix(const char *name, STRPTR base, LONG baselen);
int lhh_name_ends_info(const char *name);
int lhh_name_ends_lha(const char *name);
int lhh_path_crosses_lha(const char *path);
int lhh_is_disk_info_name(const char *name);
int lhh_path_next(const char **path, char *comp, LONG complen);
void lhh_real_append_name(STRPTR out, LONG outlen, const char *base,
    const char *name);
void lhh_real_join(STRPTR out, LONG outlen, const char *base, const char *name);
void lhh_virt_append_name(STRPTR out, LONG outlen, const char *base,
    const char *name);
int lhh_virt_parent_path(const char *virt, STRPTR out, LONG outlen);
int lhh_real_to_virt_path(GD gd, const char *real, STRPTR virt, LONG outlen);
void lhh_examine_fib_name(struct FileInfoBlock *fib, const char *virt);

/*
 * RKRM 14.3.1: handler ACTION_EXAMINE_* replies use BSTRs in fib_FileName
 * and fib_Comment; dos.library converts them to C strings for Examine().
 * Call after LhExamine/LhExNext (which fill C strings like dos.library).
 */
void lhh_fib_cstr_to_bstr(struct FileInfoBlock *fib);
int lhh_virt_to_real(GD gd, const char *virt, STRPTR real, LONG reallen);
int lhh_split_archive(GD gd, const char *virt,
    STRPTR real_out, LONG real_len,
    STRPTR entry_out, LONG entry_len);
void lhh_file_part(const char *path, STRPTR out, LONG outlen);
int lhh_parent_path(const char *path, STRPTR out, LONG outlen);
int lhh_cstr_has(const char *s, char ch);
int lhh_canon_volume(GD gd, const char *vol, STRPTR out, LONG outlen);
int lhh_is_self_volume(GD gd, const char *name);
int lhh_is_self_real(GD gd, const char *real);
int lhh_is_volume_root_name(GD gd, const char *name);

/*
 * If name begins with our device name and ':', strip that prefix in place
 * (e.g. "lha:volumename/foo" -> "volumename/foo").  Returns 1 if stripped
 * (caller should resolve from volume root, not relative to parent).
 */
int lhh_strip_self_prefix(GD gd, char *name);

/*
 * Non-blocking DosList lock for use inside the handler (RKRM: never
 * LockDosList from a handler - it can deadlock).  Spins AttemptLockDosList
 * briefly.  Returns NULL if unavailable; only UnLockDosList if non-NULL.
 */
struct DosList *lhh_attempt_lock_doslist(GD gd, ULONG flags);

/* lh_arc.c */
int lhh_lock_valid(GD gd, struct LhHLock *lock);
void lhh_arc_init(GD gd);
struct LhHArc *lhh_arc_obtain(GD gd, const char *real_path, LONG *err);
void lhh_arc_release(GD gd, struct LhHArc *arc);
int lhh_arc_refresh(GD gd, struct LhHArc *arc, LONG *err);
struct LhHLock *lhh_lock_alloc(GD gd, ULONG type, struct LhHArc *arc,
    const char *entry, BPTR real_lock, const char *real_path,
    const char *virt_path, LONG access, LONG *err);
void lhh_lock_free(GD gd, struct LhHLock *lock);
struct LhHLock *lhh_lock_dup(GD gd, struct LhHLock *src, LONG *err);
struct LhHLock *lhh_lock_parent(GD gd, struct LhHLock *lock, LONG *err);
struct LhHLock *lhh_lock_resolve(GD gd, struct LhHLock *parent,
    STRPTR name_bstr, LONG access, LONG *err);
struct LhHHandle *lhh_handle_open(GD gd, struct LhHLock *parent,
    STRPTR name_bstr, LONG mode, LONG *err);
/* ACTION_FH_FROM_LOCK: open from an existing lock (lock stolen on success). */
struct LhHHandle *lhh_handle_from_lock(GD gd, struct LhHLock *lock,
    LONG mode, LONG *err);
void lhh_handle_close(GD gd, struct LhHHandle *hh);
LONG lhh_handle_read(GD gd, struct LhHHandle *hh, APTR buf, LONG len);
LONG lhh_handle_write(GD gd, struct LhHHandle *hh, APTR buf, LONG len);
LONG lhh_handle_seek(GD gd, struct LhHHandle *hh, LONG pos, LONG mode);
LONG lhh_delete_name(GD gd, struct LhHLock *parent, STRPTR name_bstr, LONG *err);
LONG lhh_rename_name(GD gd, struct LhHLock *from_parent, STRPTR from_bstr,
    struct LhHLock *to_parent, STRPTR to_bstr, LONG *err);
struct LhHLock *lhh_create_dir_name(GD gd, struct LhHLock *parent,
    STRPTR name_bstr, LONG *err);

/* lh_pkt.c - host FS via SendPkt + private reply (never waits on gd_Port). */
void lhh_host_pkt_cleanup(void);
BPTR lhh_host_lock(STRPTR name, LONG mode);
BPTR lhh_host_lock_from(BPTR parent, STRPTR name, LONG mode);
LONG lhh_host_unlock(BPTR lock);
BPTR lhh_host_duplock(BPTR lock);
BPTR lhh_host_parentdir(BPTR lock);
LONG lhh_host_examine(BPTR lock, struct FileInfoBlock *fib);
LONG lhh_host_exnext(BPTR lock, struct FileInfoBlock *fib);
LONG lhh_host_exall(BPTR lock, STRPTR buffer, LONG size, LONG type,
    struct ExAllControl *ec);
LONG lhh_host_info(BPTR lock, struct InfoData *info);
BPTR lhh_host_open(STRPTR name, LONG mode);
LONG lhh_host_close(BPTR fh_bptr);
LONG lhh_host_read(BPTR fh_bptr, APTR buf, LONG len);
LONG lhh_host_write(BPTR fh_bptr, APTR buf, LONG len);
LONG lhh_host_seek(BPTR fh_bptr, LONG pos, LONG mode);
LONG lhh_host_delete(STRPTR name);
LONG lhh_host_rename(STRPTR oldpath, STRPTR newpath);

/* lh_volume.c - DLT_VOLUME publish for Workbench */
void lhh_volume_init(GD gd);
int lhh_volume_try_add(GD gd);
void lhh_volume_remove(GD gd);
BPTR lhh_volume_bptr(GD gd);
int lhh_is_self_volume_entry(GD gd, STRPTR bname);

#endif /* LH_HANDLER_H */
