/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_pkt.c - Safe dos packets from inside the handler.
 *
 * Host FS access uses SendPkt to the *other* handler with a private reply
 * port so we never WaitPort on the handler's pr_MsgPort (LHA: packets).
 * Volumes are resolved via LockDosList; never GetDeviceProc() (uses
 * pr_MsgPort and deadlocks from inside a handler).
 */

#include "lh-handler.h"

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

/*
 * Volume/assign lookup via DeviceProc.  Never call GetDeviceProc() from a
 * handler: it waits on pr_MsgPort, the same port used for LHA: packets.
 *
 * DeviceProc returns the handler MsgPort.  For assigns, IoErr() holds a
 * directory lock (do not UnLock it).  For volumes, IoErr() is typically 0.
 */
struct LhhHostVol {
    struct MsgPort *port;
    BPTR start_lock;  /* assign lock from IoErr(), or ZERO for volume root */
};

/*
 * Resolve "Vol:rest" via DeviceProc.  DeviceProc returns the handler port
 * and, for assigns, leaves a directory lock in IoErr() (do not UnLock it;
 * DupLock if needed).  Never GetDeviceProc (waits on pr_MsgPort).
 * Never use dol_LockList as a directory lock.
 */
static int lhh_host_resolve_name(STRPTR name, struct LhhHostVol *hv,
    const char **rest_out)
{
    struct MsgPort *port;
    const char *rest;
    LONG i;
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
    volname[i] = ':';
    volname[i + 1] = '\0';
    rest = name;
    if (name[i] == ':') {
        rest = name + i + 1;
    } else {
        /* "AmigaZen" with no colon - still DeviceProc("AmigaZen:") */
        rest = name + i;
    }

    port = DeviceProc((STRPTR)volname);
    if (port == NULL) {
        return 0;
    }
    hv->port = port;
    /*
     * For assigns, IoErr() holds a lock on the assigned directory.
     * For volumes/devices it is typically ZERO (use LOCATE "" with Arg1=0).
     */
    hv->start_lock = (BPTR)IoErr();
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
 * Send one packet to handlerport and wait on a private reply port.
 * Returns dp_Res1; *res2 receives dp_Res2 when non-NULL.
 */
static LONG lhh_dopkt(struct MsgPort *handlerport, LONG action,
    LONG arg1, LONG arg2, LONG arg3, LONG arg4, LONG arg5, LONG *res2)
{
    struct MsgPort *reply;
    struct StandardPacket *sp;
    struct DosPacket *pkt;
    LONG res1;

    if (handlerport == NULL) {
        if (res2) {
            *res2 = ERROR_DEVICE_NOT_MOUNTED;
        }
        return DOSFALSE;
    }

    reply = CreateMsgPort();
    if (reply == NULL) {
        if (res2) {
            *res2 = ERROR_NO_FREE_STORE;
        }
        return DOSFALSE;
    }

    sp = (struct StandardPacket *)AllocMem(sizeof(struct StandardPacket),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (sp == NULL) {
        DeleteMsgPort(reply);
        if (res2) {
            *res2 = ERROR_NO_FREE_STORE;
        }
        return DOSFALSE;
    }

    pkt = &sp->sp_Pkt;
    sp->sp_Msg.mn_Node.ln_Name = (char *)pkt;
    sp->sp_Msg.mn_Length = (UWORD)sizeof(struct StandardPacket);
    sp->sp_Msg.mn_ReplyPort = reply;

    pkt->dp_Link = &sp->sp_Msg;
    pkt->dp_Port = reply;
    pkt->dp_Type = action;
    pkt->dp_Arg1 = arg1;
    pkt->dp_Arg2 = arg2;
    pkt->dp_Arg3 = arg3;
    pkt->dp_Arg4 = arg4;
    pkt->dp_Arg5 = arg5;

    PutMsg(handlerport, &sp->sp_Msg);
    WaitPort(reply);
    GetMsg(reply);

    res1 = pkt->dp_Res1;
    if (res2 != NULL) {
        *res2 = pkt->dp_Res2;
    }

    FreeMem(sp, sizeof(struct StandardPacket));
    DeleteMsgPort(reply);
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
 */
BPTR lhh_host_lock_from(BPTR parent, STRPTR name, LONG mode)
{
    struct FileLock *fl;
    BPTR bstr;
    LONG res2;
    BPTR result;

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
    bstr = lhh_make_bstr(lhh_bbuf, name);
    result = (BPTR)lhh_dopkt(fl->fl_Task, ACTION_LOCATE_OBJECT,
        (LONG)parent, (LONG)bstr, mode, 0, 0, &res2);
    if (result == ZERO) {
        SetIoErr(res2);
    }
    return result;
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

    lhh_host_split_dir_file(rest, dirpart, (LONG)sizeof(dirpart),
        filepart, (LONG)sizeof(filepart));
    if (dirpart[0] != '\0') {
        dirlock = lhh_host_walk(hv.port, root, dirpart, SHARED_LOCK, &res2);
        lhh_host_unlock(root);
        root = ZERO;
    } else {
        dirlock = root;
    }
    if (dirlock == ZERO) {
        if (root != ZERO) {
            lhh_host_unlock(root);
        }
        SetIoErr(res2);
        return ZERO;
    }

    bstr = lhh_make_bstr(lhh_bbuf, filepart);
    {
        struct FileHandle *fh;

        fh = (struct FileHandle *)AllocMem(sizeof(struct FileHandle),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (fh == NULL) {
            lhh_host_unlock(dirlock);
            SetIoErr(ERROR_NO_FREE_STORE);
            return ZERO;
        }

        result = (BPTR)lhh_dopkt(hv.port, action,
            (LONG)MKBADDR(fh), (LONG)dirlock, (LONG)bstr, 0, 0, &res2);
        lhh_host_unlock(dirlock);
        if (result == DOSFALSE) {
            SetIoErr(res2);
            FreeMem(fh, sizeof(struct FileHandle));
            return ZERO;
        }
        if (fh->fh_Type == NULL) {
            SetIoErr(ERROR_OBJECT_NOT_FOUND);
            FreeMem(fh, sizeof(struct FileHandle));
            return ZERO;
        }
        return MKBADDR(fh);
    }
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
