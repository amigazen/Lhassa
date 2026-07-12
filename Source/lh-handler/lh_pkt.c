/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_pkt.c - Safe dos packets from inside the handler.
 *
 * Host FS access mirrors HappyENV's DoPacket pattern: PutMsg to the *other*
 * handler's fl_Task / dol_Task, WaitPort on a *private* reply port (never
 * pr_MsgPort / gd_Port — that would deadlock with LHA: client packets).
 * Volumes via DosList; DeviceProc only as fallback (never GetDeviceProc).
 *
 * HappyENV opens ENVARC files with one LOCATE/FINDINPUT using the full
 * relative path as a single BSTR from the assign root.  We try that first,
 * then fall back to per-component walks for handlers that reject paths.
 */

#include "lh-handler.h"

#include <exec/nodes.h>
#include <dos/dosextens.h>

LONG lhh_host_unlock(BPTR lock);

/*
 * This file has no GD parameter.  Redirect the root-handler-style base
 * macros to LhhGD (set in lhh_main before any host FS call).
 */
#ifdef __SASC
#undef SysBase
#undef DOSBase
#undef UtilityBase
#define gd LhhGD
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#define UtilityBase gd->gd_UtilityBase
#endif

/* Shared BSTR scratch - handler is single-threaded. */
static UBYTE lhh_bbuf[260];

/*
 * HappyENV PacketPort / MyPacket: one private reply port + StandardPacket
 * for the life of the handler (Create/Delete per call wastes mem and can fail).
 */
static struct MsgPort *lhh_pkt_reply;
static struct StandardPacket *lhh_pkt_sp;

#ifndef DLT_VOLUME
#define DLT_VOLUME      2
#endif
#ifndef DLT_DIRECTORY
#define DLT_DIRECTORY   1
#endif

/* Forward decls - used by doslist root before their definitions. */
static LONG lhh_dopkt(struct MsgPort *handlerport, LONG action,
    LONG arg1, LONG arg2, LONG arg3, LONG arg4, LONG arg5, LONG *res2);
static BPTR lhh_make_bstr(UBYTE *buf, const char *s);
BPTR lhh_host_parentdir(BPTR lock);

static int lhh_pkt_ensure(void)
{
    if (lhh_pkt_reply != NULL && lhh_pkt_sp != NULL) {
        return 1;
    }
    if (lhh_pkt_reply == NULL) {
        lhh_pkt_reply = CreateMsgPort();
        if (lhh_pkt_reply == NULL) {
            return 0;
        }
    }
    if (lhh_pkt_sp == NULL) {
        lhh_pkt_sp = (struct StandardPacket *)AllocMem(
            sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR);
        if (lhh_pkt_sp == NULL) {
            return 0;
        }
    }
    return 1;
}

void lhh_host_pkt_cleanup(void)
{
    if (lhh_pkt_sp != NULL) {
        FreeMem(lhh_pkt_sp, sizeof(struct StandardPacket));
        lhh_pkt_sp = NULL;
    }
    if (lhh_pkt_reply != NULL) {
        DeleteMsgPort(lhh_pkt_reply);
        lhh_pkt_reply = NULL;
    }
}

struct LhhHostVol {
    struct MsgPort *port;
    BPTR start_lock;  /* assign dol_Lock (do not UnLock), or ZERO */
};

/*
 * Volume/assign lookup via DosList (AttemptLockDosList).  Avoid DeviceProc
 * when possible: its IoErr() lock-vs-error ambiguity breaks nested walks,
 * and it may LockDosList.  Assigns use dol_Lock (do not UnLock it).
 */
static int lhh_host_resolve_doslist(const char *vol, LONG vlen,
    struct LhhHostVol *hv)
{
    struct DosList *dl;
    struct DosList *e;
    STRPTR bname;
    LONG n;
    LONG i;
    ULONG flags;

    if (vol == NULL || vlen <= 0 || hv == NULL || LhhGD == NULL) {
        return 0;
    }

    flags = LDF_DEVICES | LDF_VOLUMES | LDF_ASSIGNS | LDF_READ;
    dl = lhh_attempt_lock_doslist(LhhGD, flags);
    if (dl == NULL) {
        return 0;
    }

    e = dl;
    while ((e = NextDosEntry(e, flags)) != NULL) {
        bname = (STRPTR)BADDR(e->dol_Name);
        if (bname == NULL) {
            continue;
        }
        n = (LONG)((UBYTE *)bname)[0];
        if (n != vlen) {
            continue;
        }
        for (i = 0; i < n; i++) {
            char a;
            char b;

            a = bname[i + 1];
            b = vol[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (i != n) {
            continue;
        }
        if (e->dol_Task == NULL) {
            continue;
        }
        hv->port = e->dol_Task;
        /*
         * DLT_DIRECTORY (assign): dol_Lock is the assign root.
         * Volumes/devices: LOCATE from ZERO (never dol_LockList).
         */
        if (e->dol_Type == DLT_DIRECTORY && e->dol_Lock != ZERO) {
            hv->start_lock = e->dol_Lock;
        } else {
            hv->start_lock = ZERO;
        }
        UnLockDosList(flags);
        return 1;
    }
    UnLockDosList(flags);
    return 0;
}

/*
 * Resolve "Vol:rest" to handler port + optional assign start lock.
 * Prefer DosList; fall back to DeviceProc (never GetDeviceProc).
 */
static int lhh_host_resolve_name(STRPTR name, struct LhhHostVol *hv,
    const char **rest_out)
{
    struct MsgPort *port;
    const char *rest;
    LONG i;
    LONG ioe;
    static char volname[128];

    if (name == NULL || name[0] == '\0' || hv == NULL) {
        return 0;
    }
    hv->port = NULL;
    hv->start_lock = ZERO;

    i = 0;
    while (name[i] != '\0' && name[i] != ':' && i < (LONG)sizeof(volname) - 2) {
        volname[i] = name[i];
        i++;
    }
    if (volname[0] == '\0') {
        return 0;
    }
    volname[i] = '\0';
    rest = name;
    if (name[i] == ':') {
        rest = name + i + 1;
    } else {
        rest = name + i;
    }

    if (lhh_host_resolve_doslist(volname, i, hv)) {
        if (rest_out != NULL) {
            *rest_out = rest;
        }
        return 1;
    }

    /* Fallback: DeviceProc("Vol:") - clear IoErr so a stale code is not a lock. */
    volname[i] = ':';
    volname[i + 1] = '\0';
    SetIoErr(0);
    port = DeviceProc((STRPTR)volname);
    if (port == NULL) {
        return 0;
    }
    hv->port = port;
    ioe = IoErr();
    /*
     * DeviceProc: assign lock in IoErr(), else 0.  Error codes are small;
     * real BPTRs from AllocMem are rarely in the dos error range.  If
     * DosList failed we already tried; treat tiny IoErr as "no lock".
     */
    if (ioe != 0 && (ULONG)ioe > 512UL) {
        hv->start_lock = (BPTR)ioe;
    } else {
        hv->start_lock = ZERO;
    }
    if (rest_out != NULL) {
        *rest_out = rest;
    }
    return 1;
}

/*
 * Obtain a usable directory lock for the volume/assign root.  Caller must
 * unlock the result when done (it is always a fresh LOCATE/COPY_DIR).
 */
static BPTR lhh_host_root_lock(struct LhhHostVol *hv, LONG *res2)
{
    BPTR bstr;
    BPTR result;

    if (hv == NULL || hv->port == NULL) {
        if (res2) {
            *res2 = ERROR_DEVICE_NOT_MOUNTED;
        }
        return ZERO;
    }
    if (hv->start_lock != ZERO) {
        /* Assign: DupLock so caller can always FreeLock the result. */
        result = (BPTR)lhh_dopkt(hv->port, ACTION_COPY_DIR,
            (LONG)hv->start_lock, 0, 0, 0, 0, res2);
        if (result != ZERO) {
            return result;
        }
        /* Fall through to LOCATE if DupLock unsupported. */
    }
    bstr = lhh_make_bstr(lhh_bbuf, "");
    return (BPTR)lhh_dopkt(hv->port, ACTION_LOCATE_OBJECT,
        (LONG)hv->start_lock, (LONG)bstr, SHARED_LOCK, 0, 0, res2);
}

/*
 * Send one packet to handlerport and wait on the private reply port.
 * HappyENV DoPacket: PutMsg + WaitPort/GetMsg until LN_NAME is the packet.
 * Returns dp_Res1; *res2 receives dp_Res2 when non-NULL.
 */
static LONG lhh_dopkt(struct MsgPort *handlerport, LONG action,
    LONG arg1, LONG arg2, LONG arg3, LONG arg4, LONG arg5, LONG *res2)
{
    struct MsgPort *reply;
    struct StandardPacket *sp;
    struct DosPacket *pkt;
    struct Message *msg;
    LONG res1;

    if (handlerport == NULL) {
        if (res2) {
            *res2 = ERROR_DEVICE_NOT_MOUNTED;
        }
        return DOSFALSE;
    }

    if (!lhh_pkt_ensure()) {
        if (res2) {
            *res2 = ERROR_NO_FREE_STORE;
        }
        return DOSFALSE;
    }

    reply = lhh_pkt_reply;
    sp = lhh_pkt_sp;
    pkt = &sp->sp_Pkt;

    sp->sp_Msg.mn_Node.ln_Succ = NULL;
    sp->sp_Msg.mn_Node.ln_Pred = NULL;
    sp->sp_Msg.mn_Node.ln_Type = NT_MESSAGE;
    sp->sp_Msg.mn_Node.ln_Name = (char *)pkt;
    sp->sp_Msg.mn_ReplyPort = reply;
    sp->sp_Msg.mn_Length = (UWORD)sizeof(struct StandardPacket);

    pkt->dp_Link = &sp->sp_Msg;
    pkt->dp_Port = reply;
    pkt->dp_Type = action;
    pkt->dp_Arg1 = arg1;
    pkt->dp_Arg2 = arg2;
    pkt->dp_Arg3 = arg3;
    pkt->dp_Arg4 = arg4;
    pkt->dp_Arg5 = arg5;
    pkt->dp_Res1 = 0;
    pkt->dp_Res2 = 0;

    PutMsg(handlerport, &sp->sp_Msg);

    /* HappyENV GetPacket: ignore non-dos messages on the private port. */
    for (;;) {
        WaitPort(reply);
        msg = GetMsg(reply);
        if (msg == NULL) {
            continue;
        }
        if (msg->mn_Node.ln_Name == (char *)pkt) {
            break;
        }
    }

    res1 = pkt->dp_Res1;
    if (res2 != NULL) {
        *res2 = pkt->dp_Res2;
    }
    return res1;
}

/* Build an aligned BSTR in buf (buf must be at least len+4 bytes). */
static BPTR lhh_make_bstr(UBYTE *buf, const char *s)
{
    UBYTE *bstr;
    LONG len;
    LONG i;

    bstr = (UBYTE *)(((ULONG)(buf + 3)) & ~3UL);
    len = 0;
    if (s != NULL) {
        while (s[len] != '\0' && len < 255) {
            len++;
        }
    }
    bstr[0] = (UBYTE)len;
    for (i = 0; i < len; i++) {
        bstr[i + 1] = (UBYTE)s[i];
    }
    return MKBADDR(bstr);
}

/*
 * Walk a path relative to parent_lock on handlerport one component per
 * ACTION_LOCATE_OBJECT.  '/' alone means ACTION_PARENT (Amiga semantics).
 * start_lock is not unlocked here; caller owns the returned lock.
 */
static BPTR lhh_host_walk(struct MsgPort *handlerport, BPTR start_lock,
    const char *path, LONG mode, LONG *res2)
{
    BPTR cur;
    BPTR next;
    char comp[256];
    const char *rest;
    BPTR bstr;
    int owned;

    if (handlerport == NULL || path == NULL) {
        if (res2) {
            *res2 = ERROR_OBJECT_NOT_FOUND;
        }
        return ZERO;
    }
    cur = start_lock;
    owned = 0;
    rest = path;
    while (rest != NULL && rest[0] != '\0') {
        if (!lhh_path_next(&rest, comp, (LONG)sizeof(comp))) {
            break;
        }
        if (lhh_is_parent_name(comp)) {
            next = (BPTR)lhh_dopkt(handlerport, ACTION_PARENT,
                (LONG)cur, 0, 0, 0, 0, res2);
            if (owned && cur != ZERO) {
                lhh_host_unlock(cur);
                owned = 0;
            }
            cur = next;
            if (cur == ZERO) {
                return ZERO;
            }
            owned = 1;
            continue;
        }
        bstr = lhh_make_bstr(lhh_bbuf, comp);
        next = (BPTR)lhh_dopkt(handlerport, ACTION_LOCATE_OBJECT,
            (LONG)cur, (LONG)bstr, mode, 0, 0, res2);
        if (owned && cur != ZERO) {
            lhh_host_unlock(cur);
            owned = 0;
        }
        cur = next;
        if (cur == ZERO) {
            return ZERO;
        }
        owned = 1;
    }
    if (!owned) {
        if (res2) {
            *res2 = ERROR_OBJECT_NOT_FOUND;
        }
        return ZERO;
    }
    return cur;
}

/* Split "dir/sub/file" into "dir/sub" + "file" (file-only if no '/'). */
static void lhh_host_split_dir_file(const char *path, char *dir, LONG dirlen,
    char *file, LONG filelen)
{
    LONG last;
    LONG i;

    if (dir && dirlen > 0) {
        dir[0] = '\0';
    }
    if (file && filelen > 0) {
        file[0] = '\0';
    }
    if (path == NULL || file == NULL || filelen <= 0) {
        return;
    }
    last = -1;
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            last = i;
        }
    }
    if (last < 0) {
        lhh_cstr_copy(file, path, filelen);
        return;
    }
    if (dir != NULL && dirlen > 0) {
        for (i = 0; i < last && i < dirlen - 1; i++) {
            dir[i] = path[i];
        }
        dir[i] = '\0';
    }
    lhh_cstr_copy(file, path + last + 1, filelen);
}

BPTR lhh_host_lock(STRPTR name, LONG mode)
{
    struct LhhHostVol hv;
    const char *rest;
    BPTR root;
    BPTR result;
    BPTR bstr;
    LONG res2;

    if (name == NULL || name[0] == '\0') {
        return ZERO;
    }

    if (!lhh_host_resolve_name(name, &hv, &rest)) {
        return ZERO;
    }

    root = lhh_host_root_lock(&hv, &res2);
    if (root == ZERO) {
        SetIoErr(res2);
        return ZERO;
    }

    if (rest[0] == '\0') {
        /* Volume/assign root - already have a fresh lock. */
        return root;
    }

    /*
     * HappyENV CalcFullName: one LOCATE_OBJECT with the full relative path
     * as a single BSTR from the volume/assign root (FFS and most handlers).
     */
    bstr = lhh_make_bstr(lhh_bbuf, rest);
    result = (BPTR)lhh_dopkt(hv.port, ACTION_LOCATE_OBJECT,
        (LONG)root, (LONG)bstr, mode, 0, 0, &res2);
    if (result != ZERO) {
        lhh_host_unlock(root);
        return result;
    }

    /* Fallback: per-component walk for handlers that reject multi-part BSTRs. */
    result = lhh_host_walk(hv.port, root, rest, mode, &res2);
    lhh_host_unlock(root);
    if (result == ZERO) {
        SetIoErr(res2);
    }
    return result;
}

/*
 * Locate one name relative to an existing host lock (Amiga packet model).
 * Prefer this over rebuilding "Vol:path/name" strings for nested walks.
 * Name may contain '/' — try one LOCATE first (HappyENV), then walk.
 */
BPTR lhh_host_lock_from(BPTR parent, STRPTR name, LONG mode)
{
    struct FileLock *fl;
    BPTR bstr;
    LONG res2;
    BPTR result;
    int has_slash;
    LONG i;

    if (parent == ZERO || name == NULL || name[0] == '\0') {
        return ZERO;
    }
    fl = (struct FileLock *)BADDR(parent);
    if (fl == NULL || fl->fl_Task == NULL) {
        return ZERO;
    }
    if (lhh_is_parent_name(name)) {
        return lhh_host_parentdir(parent);
    }

    has_slash = 0;
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '/') {
            has_slash = 1;
            break;
        }
    }

    bstr = lhh_make_bstr(lhh_bbuf, name);
    result = (BPTR)lhh_dopkt(fl->fl_Task, ACTION_LOCATE_OBJECT,
        (LONG)parent, (LONG)bstr, mode, 0, 0, &res2);
    if (result != ZERO) {
        return result;
    }
    if (has_slash) {
        result = lhh_host_walk(fl->fl_Task, parent, name, mode, &res2);
        if (result != ZERO) {
            return result;
        }
    }
    SetIoErr(res2);
    return ZERO;
}

LONG lhh_host_unlock(BPTR lock)
{
    struct FileLock *fl;
    LONG res2;

    if (lock == ZERO) {
        return DOSFALSE;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return DOSFALSE;
    }
    return lhh_dopkt(fl->fl_Task, ACTION_FREE_LOCK,
        (LONG)lock, 0, 0, 0, 0, &res2);
}

BPTR lhh_host_duplock(BPTR lock)
{
    struct FileLock *fl;
    LONG res2;
    BPTR result;

    if (lock == ZERO) {
        return ZERO;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return ZERO;
    }
    result = (BPTR)lhh_dopkt(fl->fl_Task, ACTION_COPY_DIR,
        (LONG)lock, 0, 0, 0, 0, &res2);
    if (result == ZERO) {
        SetIoErr(res2);
    }
    return result;
}

BPTR lhh_host_parentdir(BPTR lock)
{
    struct FileLock *fl;
    LONG res2;
    BPTR result;

    if (lock == ZERO) {
        return ZERO;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return ZERO;
    }
    result = (BPTR)lhh_dopkt(fl->fl_Task, ACTION_PARENT,
        (LONG)lock, 0, 0, 0, 0, &res2);
    if (result == ZERO) {
        SetIoErr(res2);
    }
    return result;
}

LONG lhh_host_examine(BPTR lock, struct FileInfoBlock *fib)
{
    struct FileLock *fl;
    LONG res2;
    LONG res1;

    if (lock == ZERO || fib == NULL) {
        return DOSFALSE;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return DOSFALSE;
    }
    res1 = lhh_dopkt(fl->fl_Task, ACTION_EXAMINE_OBJECT,
        (LONG)lock, (LONG)MKBADDR(fib), 0, 0, 0, &res2);
    if (!res1) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_exnext(BPTR lock, struct FileInfoBlock *fib)
{
    struct FileLock *fl;
    LONG res2;
    LONG res1;

    if (lock == ZERO || fib == NULL) {
        return DOSFALSE;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return DOSFALSE;
    }
    res1 = lhh_dopkt(fl->fl_Task, ACTION_EXAMINE_NEXT,
        (LONG)lock, (LONG)MKBADDR(fib), 0, 0, 0, &res2);
    if (!res1) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_exall(BPTR lock, STRPTR buffer, LONG size, LONG type,
    struct ExAllControl *ec)
{
    struct FileLock *fl;
    LONG res2;
    LONG res1;
    struct ExAllData *ed;
    struct ExAllData *last;
    ULONG left;
    LONG elen;
    LONG nlen;
    LONG n;
    LONG i;
    UBYTE *name;
    static BPTR lhh_exall_lock;
    static struct FileInfoBlock lhh_exall_fib;
    static int lhh_exall_have;
#define LHH_EAD_OFF(f) ((UBYTE)((ULONG)&(((struct ExAllData *)0)->f)))
    static const UBYTE ead_sizes[] = {
        0,
        LHH_EAD_OFF(ed_Type),
        LHH_EAD_OFF(ed_Size),
        LHH_EAD_OFF(ed_Prot),
        LHH_EAD_OFF(ed_Days),
        LHH_EAD_OFF(ed_Comment),
        LHH_EAD_OFF(ed_OwnerUID),
        (UBYTE)sizeof(struct ExAllData)
    };
#undef LHH_EAD_OFF

    if (lock == ZERO || buffer == NULL || ec == NULL || size <= 0) {
        return DOSFALSE;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return DOSFALSE;
    }

    /* Prefer native ExAll when the host supports it. */
    res1 = lhh_dopkt(fl->fl_Task, ACTION_EXAMINE_ALL,
        (LONG)lock, (LONG)buffer, size, type, (LONG)ec, &res2);
    if (res1) {
        return DOSTRUE;
    }
    if (res2 != ERROR_ACTION_NOT_KNOWN) {
        SetIoErr(res2);
        return DOSFALSE;
    }

    /*
     * Host rejected ExAll - synthesize from Examine/ExNext.
     * FIB state is kept in statics (handler is single-threaded).
     */
    if (type < 1 || type > 7) {
        SetIoErr(ERROR_BAD_NUMBER);
        return DOSFALSE;
    }

    if (ec->eac_LastKey == 0 || lhh_exall_lock != lock || !lhh_exall_have) {
        if (!lhh_host_examine(lock, &lhh_exall_fib)) {
            lhh_exall_have = 0;
            SetIoErr(ERROR_NO_MORE_ENTRIES);
            return DOSFALSE;
        }
        if (!lhh_host_exnext(lock, &lhh_exall_fib)) {
            lhh_exall_have = 0;
            SetIoErr(ERROR_NO_MORE_ENTRIES);
            return DOSFALSE;
        }
        lhh_exall_lock = lock;
        lhh_exall_have = 1;
    }

    ed = (struct ExAllData *)buffer;
    last = NULL;
    left = (ULONG)size;
    ec->eac_Entries = 0;

    for (;;) {
        name = (UBYTE *)lhh_exall_fib.fib_FileName;
        n = (LONG)name[0];
        if (n > 106) {
            n = 106;
        }
        nlen = (LONG)(((n + 1) + 3) & ~3);
        elen = (LONG)ead_sizes[type] + nlen;
        if (left <= (ULONG)elen) {
            ec->eac_LastKey = 1;
            if (last != NULL) {
                last->ed_Next = NULL;
            }
            return DOSTRUE;
        }
        left -= (ULONG)elen;
        switch (type) {
        case ED_OWNER:
            ed->ed_OwnerUID = lhh_exall_fib.fib_OwnerUID;
            ed->ed_OwnerGID = lhh_exall_fib.fib_OwnerGID;
        case ED_COMMENT:
            ed->ed_Comment = NULL;
        case ED_DATE:
            ed->ed_Days = lhh_exall_fib.fib_Date.ds_Days;
            ed->ed_Mins = lhh_exall_fib.fib_Date.ds_Minute;
            ed->ed_Ticks = lhh_exall_fib.fib_Date.ds_Tick;
        case ED_PROTECTION:
            ed->ed_Prot = lhh_exall_fib.fib_Protection;
        case ED_SIZE:
            ed->ed_Size = lhh_exall_fib.fib_Size;
        case ED_TYPE:
            ed->ed_Type = lhh_exall_fib.fib_DirEntryType;
        case ED_NAME:
            ed->ed_Name = (STRPTR)(ed + 1);
            break;
        }
        for (i = 0; i < n; i++) {
            ed->ed_Name[i] = (char)name[i + 1];
        }
        ed->ed_Name[n] = '\0';
        last = ed;
        ed = (struct ExAllData *)(((STRPTR)ed) + elen);
        last->ed_Next = ed;
        ec->eac_Entries++;

        if (!lhh_host_exnext(lock, &lhh_exall_fib)) {
            if (last != NULL) {
                last->ed_Next = NULL;
            }
            ec->eac_LastKey = 0;
            lhh_exall_have = 0;
            SetIoErr(ERROR_NO_MORE_ENTRIES);
            return DOSFALSE;
        }
    }
}

LONG lhh_host_info(BPTR lock, struct InfoData *info)
{
    struct FileLock *fl;
    LONG res2;
    LONG res1;

    if (lock == ZERO || info == NULL) {
        return DOSFALSE;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return DOSFALSE;
    }
    res1 = lhh_dopkt(fl->fl_Task, ACTION_INFO,
        (LONG)lock, (LONG)MKBADDR(info), 0, 0, 0, &res2);
    if (!res1) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_namefromlock(BPTR lock, STRPTR buffer, LONG len)
{
    struct FileLock *fl;
    LONG res2;
    LONG res1;

    if (lock == ZERO || buffer == NULL || len <= 0) {
        return DOSFALSE;
    }
    fl = (struct FileLock *)BADDR(lock);
    if (fl == NULL || fl->fl_Task == NULL) {
        return DOSFALSE;
    }
    res1 = lhh_dopkt(fl->fl_Task, ACTION_COPY_DIR_FH,
        (LONG)lock, (LONG)buffer, len, 0, 0, &res2);
    /*
     * ACTION_COPY_DIR_FH is wrong for NameFromLock.  Use ACTION_LOCATE_OBJECT
     * style is not right either.  NameFromLock is ACTION_COPY_DIR? No.
     *
     * Correct packet is ACTION_NAME_FROM_LOCK? In dos, NameFromLock uses
     * internal code.  Packet type ACTION_COPY_DIR is DupLock.
     *
     * From dos packets: there is no portable NAME_FROM_LOCK packet on all
     * versions.  Fall back to dos.library NameFromLock only when safe �
     * we use pr_MsgPort for the handler, so NameFromLock is unsafe.
     *
     * Use ACTION_EXAMINE_OBJECT and read fib_FileName as a weak fallback,
     * or implement via the packet ACTION_CURRENT_VOLUME + path walk.
     *
     * AmigaOS 2.0+ NameFromLock sends nothing special � it walks fl_Key
     * in the handler.  The packet is not public on all FS.
     *
     * Practical approach: store real_path on every REAL lock and never
     * call NameFromLock.  Parent uses path string math.
     */
    (void)res1;
    (void)res2;
    buffer[0] = '\0';
    return DOSFALSE;
}

BPTR lhh_host_open(STRPTR name, LONG mode)
{
    struct LhhHostVol hv;
    const char *rest;
    LONG res2;
    BPTR result;
    LONG action;
    static char dirpart[256];
    static char filepart[256];
    BPTR root;
    BPTR dirlock;
    BPTR bstr;
    struct FileHandle *fh;

    if (name == NULL || name[0] == '\0') {
        return ZERO;
    }

    if (mode == MODE_NEWFILE) {
        action = ACTION_FINDOUTPUT;
    } else if (mode == MODE_READWRITE) {
        action = ACTION_FINDUPDATE;
    } else {
        action = ACTION_FINDINPUT;
    }

    if (!lhh_host_resolve_name(name, &hv, &rest)) {
        return ZERO;
    }

    root = lhh_host_root_lock(&hv, &res2);
    if (root == ZERO) {
        SetIoErr(res2);
        return ZERO;
    }

    fh = (struct FileHandle *)AllocMem(sizeof(struct FileHandle),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (fh == NULL) {
        lhh_host_unlock(root);
        SetIoErr(ERROR_NO_FREE_STORE);
        return ZERO;
    }

    /*
     * HappyENV: FINDINPUT from assign/volume root with the full relative
     * path as one BSTR (e.g. "sys/def_drawer.info" on ENV:).
     */
    if (rest[0] != '\0') {
        bstr = lhh_make_bstr(lhh_bbuf, rest);
        result = (BPTR)lhh_dopkt(hv.port, action,
            (LONG)MKBADDR(fh), (LONG)root, (LONG)bstr, 0, 0, &res2);
        if (result != DOSFALSE && fh->fh_Type != NULL) {
            lhh_host_unlock(root);
            return MKBADDR(fh);
        }
        /* Clear FH for retry; some handlers partially fill on failure. */
        fh->fh_Type = NULL;
        fh->fh_Port = NULL;
        fh->fh_Arg1 = 0;
    }

    /* Fallback: walk to parent dir, FIND leaf name only. */
    lhh_host_split_dir_file(rest, dirpart, (LONG)sizeof(dirpart),
        filepart, (LONG)sizeof(filepart));
    if (dirpart[0] != '\0') {
        dirlock = lhh_host_walk(hv.port, root, dirpart, SHARED_LOCK, &res2);
        lhh_host_unlock(root);
        root = ZERO;
    } else {
        dirlock = root;
        root = ZERO;
    }
    if (dirlock == ZERO) {
        FreeMem(fh, sizeof(struct FileHandle));
        SetIoErr(res2);
        return ZERO;
    }
    if (filepart[0] == '\0') {
        lhh_host_unlock(dirlock);
        FreeMem(fh, sizeof(struct FileHandle));
        SetIoErr(ERROR_OBJECT_WRONG_TYPE);
        return ZERO;
    }

    bstr = lhh_make_bstr(lhh_bbuf, filepart);
    result = (BPTR)lhh_dopkt(hv.port, action,
        (LONG)MKBADDR(fh), (LONG)dirlock, (LONG)bstr, 0, 0, &res2);
    lhh_host_unlock(dirlock);
    if (result == DOSFALSE || fh->fh_Type == NULL) {
        if (result == DOSFALSE) {
            SetIoErr(res2);
        } else {
            SetIoErr(ERROR_OBJECT_NOT_FOUND);
        }
        FreeMem(fh, sizeof(struct FileHandle));
        return ZERO;
    }
    return MKBADDR(fh);
}

LONG lhh_host_close(BPTR fh_bptr)
{
    struct FileHandle *fh;
    LONG res2;
    LONG res1;

    if (fh_bptr == ZERO) {
        return DOSFALSE;
    }
    fh = (struct FileHandle *)BADDR(fh_bptr);
    if (fh == NULL || fh->fh_Type == NULL) {
        return DOSFALSE;
    }
    res1 = lhh_dopkt(fh->fh_Type, ACTION_END,
        fh->fh_Arg1, 0, 0, 0, 0, &res2);
    FreeMem(fh, sizeof(struct FileHandle));
    if (!res1) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_read(BPTR fh_bptr, APTR buf, LONG len)
{
    struct FileHandle *fh;
    LONG res2;
    LONG res1;

    if (fh_bptr == ZERO || buf == NULL || len <= 0) {
        return -1;
    }
    fh = (struct FileHandle *)BADDR(fh_bptr);
    if (fh == NULL || fh->fh_Type == NULL) {
        return -1;
    }
    res1 = lhh_dopkt(fh->fh_Type, ACTION_READ,
        fh->fh_Arg1, (LONG)buf, len, 0, 0, &res2);
    if (res1 < 0) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_write(BPTR fh_bptr, APTR buf, LONG len)
{
    struct FileHandle *fh;
    LONG res2;
    LONG res1;

    if (fh_bptr == ZERO || buf == NULL || len <= 0) {
        return -1;
    }
    fh = (struct FileHandle *)BADDR(fh_bptr);
    if (fh == NULL || fh->fh_Type == NULL) {
        return -1;
    }
    res1 = lhh_dopkt(fh->fh_Type, ACTION_WRITE,
        fh->fh_Arg1, (LONG)buf, len, 0, 0, &res2);
    if (res1 < 0) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_seek(BPTR fh_bptr, LONG pos, LONG mode)
{
    struct FileHandle *fh;
    LONG res2;
    LONG res1;

    if (fh_bptr == ZERO) {
        return -1;
    }
    fh = (struct FileHandle *)BADDR(fh_bptr);
    if (fh == NULL || fh->fh_Type == NULL) {
        return -1;
    }
    res1 = lhh_dopkt(fh->fh_Type, ACTION_SEEK,
        fh->fh_Arg1, pos, mode, 0, 0, &res2);
    if (res1 < 0) {
        SetIoErr(res2);
    }
    return res1;
}

LONG lhh_host_delete(STRPTR name)
{
    struct LhhHostVol hv;
    const char *rest;
    LONG res2;
    LONG res1;
    static char dirpart[256];
    static char filepart[256];
    BPTR root;
    BPTR dirlock;
    BPTR bstr;

    if (name == NULL || name[0] == '\0') {
        return DOSFALSE;
    }

    if (!lhh_host_resolve_name(name, &hv, &rest)) {
        return DOSFALSE;
    }

    root = lhh_host_root_lock(&hv, &res2);
    if (root == ZERO) {
        SetIoErr(res2);
        return DOSFALSE;
    }

    /* HappyENV-style: DELETE with full relative path from root. */
    if (rest[0] != '\0') {
        bstr = lhh_make_bstr(lhh_bbuf, rest);
        res1 = lhh_dopkt(hv.port, ACTION_DELETE_OBJECT,
            (LONG)root, (LONG)bstr, 0, 0, 0, &res2);
        if (res1) {
            lhh_host_unlock(root);
            return res1;
        }
    }

    lhh_host_split_dir_file(rest, dirpart, (LONG)sizeof(dirpart),
        filepart, (LONG)sizeof(filepart));
    if (dirpart[0] != '\0') {
        dirlock = lhh_host_walk(hv.port, root, dirpart, SHARED_LOCK, &res2);
        lhh_host_unlock(root);
        if (dirlock == ZERO) {
            SetIoErr(res2);
            return DOSFALSE;
        }
    } else {
        dirlock = root;
    }

    bstr = lhh_make_bstr(lhh_bbuf, filepart);
    res1 = lhh_dopkt(hv.port, ACTION_DELETE_OBJECT,
        (LONG)dirlock, (LONG)bstr, 0, 0, 0, &res2);
    lhh_host_unlock(dirlock);
    if (!res1) {
        SetIoErr(res2);
    }
    return res1;
}

/*
 * Host Rename via ACTION_RENAME_OBJECT on the volume handler (private reply).
 * oldpath and newpath are full Amiga paths on the same volume.
 */
LONG lhh_host_rename(STRPTR oldpath, STRPTR newpath)
{
    struct LhhHostVol hv;
    struct LhhHostVol hv2;
    const char *rest;
    const char *rest2;
    LONG res2;
    LONG res1;
    BPTR root;
    BPTR root2;
    BPTR bstr_from;
    BPTR bstr_to;
    static UBYTE bbuf2[260];

#ifndef ERROR_RENAME_ACROSS_DEVICES
#define ERROR_RENAME_ACROSS_DEVICES 215
#endif

    if (oldpath == NULL || newpath == NULL
        || oldpath[0] == '\0' || newpath[0] == '\0') {
        return DOSFALSE;
    }
    if (!lhh_host_resolve_name(oldpath, &hv, &rest)) {
        return DOSFALSE;
    }
    if (!lhh_host_resolve_name(newpath, &hv2, &rest2)) {
        return DOSFALSE;
    }
    if (hv.port != hv2.port) {
        SetIoErr(ERROR_RENAME_ACROSS_DEVICES);
        return DOSFALSE;
    }
    root = lhh_host_root_lock(&hv, &res2);
    if (root == ZERO) {
        SetIoErr(res2);
        return DOSFALSE;
    }
    root2 = lhh_host_root_lock(&hv2, &res2);
    if (root2 == ZERO) {
        lhh_host_unlock(root);
        SetIoErr(res2);
        return DOSFALSE;
    }
    bstr_from = lhh_make_bstr(lhh_bbuf, rest[0] ? rest : "");
    bstr_to = lhh_make_bstr(bbuf2, rest2[0] ? rest2 : "");
    res1 = lhh_dopkt(hv.port, ACTION_RENAME_OBJECT,
        (LONG)root, (LONG)bstr_from, (LONG)root2, (LONG)bstr_to, 0, &res2);
    lhh_host_unlock(root);
    lhh_host_unlock(root2);
    if (!res1) {
        SetIoErr(res2);
    }
    return res1;
}
