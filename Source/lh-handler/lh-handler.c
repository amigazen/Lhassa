/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh-handler.c - LHA: overlay (root volume list + real FS walk + archives).
 *
 * Incoming LHA: packets use pr_MsgPort (root-handler).  Host FS uses
 * lhh_host_* (SendPkt + private reply).  Large path buffers are static -
 * dos handlers have a tiny stack; 1KB+ locals in locate corrupt the process
 * and every lock returns object not found.
 *
 * Amiga path rules (not POSIX): ':' is volume root; each locate carries one
 * name component; '/' alone means parent.  Virtual paths under LHA: never
 * contain ':': LHA:AmigaZen maps to AmigaZen:, LHA:AmigaZen: is invalid.
 * LHA:AmigaZen/lhasa/file.lha/entry maps to AmigaZen:lhasa/file.lha + entry.
 */

#include "lh-handler.h"

#include <string.h>

struct Library *LhBase = NULL;
GD LhhGD = NULL;

const UBYTE version[] = VERSTAG;
const UBYTE handler_name[] = NAME;

#define EADOffset(field) ((UBYTE)&(((struct ExAllData *)NULL)->field))
static const UBYTE sizes[] = {
    0,
    EADOffset(ed_Type),
    EADOffset(ed_Size),
    EADOffset(ed_Prot),
    EADOffset(ed_Days),
    EADOffset(ed_Comment),
    EADOffset(ed_OwnerUID),
    sizeof(struct ExAllData)
};

static ULONG lhh_main(void);

RegCall GetA4 ULONG entry(REGA0 UBYTE *line, REGD0 ULONG len)
{
    (void)line;
    (void)len;
    return lhh_main();
}

static struct DosPacket *WaitDosPacket(GD gd)
{
    WaitPort(gd->gd_Port);
    return (struct DosPacket *)(GetMsg(gd->gd_Port)->mn_Node.ln_Name);
}

static void ReplyDosPacket1(GD gd, struct DosPacket *Packet, LONG Res1)
{
    struct MsgPort *reply;

    reply = Packet->dp_Port;
    Packet->dp_Port = gd->gd_Port;
    Packet->dp_Link->mn_Node.ln_Name = (char *)Packet;
    Packet->dp_Res1 = Res1;
    PutMsg(reply, Packet->dp_Link);
}

static void ReplyDosPacket2(GD gd, struct DosPacket *Packet, LONG Res1, LONG Res2)
{
    Packet->dp_Res2 = Res2;
    ReplyDosPacket1(gd, Packet, Res1);
}

static BOOL openres(GD gd, LONG *err)
{
    BOOL rv;

    rv = FALSE;
    DOSBase = NULL;
    UtilityBase = NULL;
    LhBase = NULL;
    gd->gd_LhBase = NULL;
    gd->gd_Pool = NULL;
    gd->gd_UsageCnt = 0;
    gd->gd_LockCnt = 0;
    lhh_arc_init(gd);

    if (SysBase->lib_Version >= 39) {
        if ((DOSBase = OpenLibrary((STRPTR)"dos.library", 39)) != NULL) {
            if ((gd->gd_Pool = CreatePool(MEMF_CLEAR | MEMF_PUBLIC,
                    LHH_PUDDLE, LHH_PUDDLE)) != NULL) {
                if ((UtilityBase = OpenLibrary((STRPTR)"utility.library",
                        39)) != NULL) {
                    LhBase = OpenLibrary((STRPTR)LH_NAME, LH_MIN_VERSION);
                    gd->gd_LhBase = LhBase;
                    /* Mount even if lh.library is missing (volume list still works). */
                    rv = TRUE;
                } else if (err) {
                    *err = ERROR_OBJECT_NOT_FOUND;
                }
            } else if (err) {
                *err = ERROR_NO_FREE_STORE;
            }
        } else if (err) {
            *err = ERROR_OBJECT_NOT_FOUND;
        }
    } else if (err) {
        *err = ERROR_OBJECT_NOT_FOUND;
    }
    return rv;
}

static void closeres(GD gd)
{
    if (LhBase != NULL) {
        CloseLibrary(LhBase);
        LhBase = NULL;
        gd->gd_LhBase = NULL;
    }
    if (UtilityBase != NULL) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    if (gd->gd_Pool != NULL) {
        DeletePool(gd->gd_Pool);
        gd->gd_Pool = NULL;
    }
    if (DOSBase != NULL) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

static BOOL Die(GD gd)
{
    if (gd->gd_UsageCnt != 0) {
        return FALSE;
    }
    if (gd->gd_Port != NULL && !IsListEmpty(&gd->gd_Port->mp_MsgList)) {
        return FALSE;
    }
    return TRUE;
}

/* Skip our own LHA: entry in the root volume list. */
static int IsSelfDosEntry(GD gd, STRPTR bname)
{
    STRPTR self;
    LONG n;
    LONG i;

    if (gd->gd_DosList == NULL || bname == NULL) {
        return 0;
    }
    self = (STRPTR)BADDR(gd->gd_DosList->dol_Name);
    if (self == NULL) {
        return 0;
    }
    n = (LONG)((UBYTE *)self)[0];
    if (n != (LONG)((UBYTE *)bname)[0]) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char a;
        char b;

        a = self[i + 1];
        b = bname[i + 1];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static struct LhHLock *pkt_lock(GD gd, BPTR bptr)
{
    struct LhHLock *lock;

    if (bptr == ZERO) {
        return NULL;
    }
    lock = (struct LhHLock *)BADDR(bptr);
    if (!lhh_lock_valid(gd, lock)) {
        return NULL;
    }
    return lock;
}

static void examine_root_fib(struct FileInfoBlock *fib)
{
    fib->fib_DirEntryType = ST_ROOT;
    fib->fib_EntryType = ST_ROOT;
    fib->fib_DiskKey = 0;
    fib->fib_Protection = 0;
    fib->fib_Size = 0;
    fib->fib_NumBlocks = 0;
    fib->fib_FileName[0] = 3;
    fib->fib_FileName[1] = 'L';
    fib->fib_FileName[2] = 'H';
    fib->fib_FileName[3] = 'A';
    fib->fib_FileName[4] = '\0';
    fib->fib_Comment[0] = 0;
}

static void examine_archive_fib(struct LhHLock *lock, struct FileInfoBlock *fib)
{
    fib->fib_DirEntryType = ST_USERDIR;
    fib->fib_EntryType = ST_USERDIR;
    fib->fib_DiskKey = 0;
    fib->fib_Protection = 0;
    fib->fib_Size = 0;
    fib->fib_NumBlocks = 0;
    fib->fib_Comment[0] = 0;
    lhh_examine_fib_name(fib, lock->virt_path);
}

static ULONG lhh_main(void)
{
    struct GlobalData GlobalData;
    struct GlobalData *gd;
    struct DosPacket *Pkt;
    BOOL Done;
    LONG err;

    gd = &GlobalData;
    LhhGD = gd;
    err = ERROR_NO_FREE_STORE;
    SysBase = *(struct Library **)4L;

    if ((gd->gd_We = (struct Process *)FindTask(NULL))->pr_CLI != NULL) {
        return RETURN_FAIL;
    }

    /* root-handler: dos packets on pr_MsgPort only */
    gd->gd_Port = &gd->gd_We->pr_MsgPort;
    Pkt = WaitDosPacket(gd);

    if (!openres(gd, &err)) {
        DB1("openres fail err=%ld\n", err);
        ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
        closeres(gd);
        return RETURN_FAIL;
    }

    gd->gd_DosList = (struct DosList *)BADDR(Pkt->dp_Arg3);
    gd->gd_DosList->dol_Task = gd->gd_Port;
    ReplyDosPacket1(gd, Pkt, DOSTRUE);

    /* Host I/O uses DosList + private reply port, not GetDeviceProc. */
    lhh_log_open(LHH_LOG_PATH);
    if (lhh_log_active()) {
        DB1("handler ready log=%s\n", lhh_log_path_used());
    } else {
        /* Still emit so ring holds a clue if open later succeeds. */
        DB("handler ready log=FAILED");
    }

    Done = FALSE;
    while (Done == FALSE) {
        Pkt = WaitDosPacket(gd);
        Pkt->dp_Res1 = DOSFALSE;
        Pkt->dp_Res2 = ERROR_ACTION_NOT_KNOWN;

        lhh_db_pkt_enter();
        switch (Pkt->dp_Type) {
        case ACTION_DIE:
            if ((Done = Die(gd)) == TRUE) {
                ReplyDosPacket1(gd, Pkt, DOSTRUE);
            } else {
                ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_OBJECT_IN_USE);
            }
            break;

        case ACTION_IS_FILESYSTEM:
            ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
            break;

        case ACTION_SAME_LOCK:
            {
                struct LhHLock *a;
                struct LhHLock *b;

                a = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                b = pkt_lock(gd, (BPTR)Pkt->dp_Arg2);
                if (a == NULL || b == NULL || a->type != b->type) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, 0);
                    break;
                }
                if (a->type == LHH_LOCK_ROOT) {
                    ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    break;
                }
                if (a->type == LHH_LOCK_REAL
                    || a->type == LHH_LOCK_ARCHIVE) {
                    if (lhh_cstr_eq_i(a->real_path, b->real_path)
                        && lhh_cstr_eq_i(a->virt_path, b->virt_path)) {
                        ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE, 0);
                    }
                    break;
                }
                if (a->type == LHH_LOCK_ENTRY
                    && lhh_cstr_eq_i(a->real_path, b->real_path)
                    && lhh_cstr_eq_i(a->virt_path, b->virt_path)
                    && lhh_cstr_eq_i(a->entry, b->entry)) {
                    ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                } else {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, 0);
                }
            }
            break;

        case ACTION_LOCATE_OBJECT:
            {
                struct LhHLock *parent;
                struct LhHLock *lock;
                LONG mode;
                STRPTR nameb;
                static char locname[LHH_PATH_LEN];

                mode = Pkt->dp_Arg3;
                if (mode != EXCLUSIVE_LOCK) {
                    mode = SHARED_LOCK;
                }
                parent = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                nameb = NULL;
                if (Pkt->dp_Arg2 != ZERO) {
                    nameb = (STRPTR)BADDR(Pkt->dp_Arg2);
                }
                lhh_bstr_to_cstr(nameb, locname, LHH_PATH_LEN);
                /* Trailing '/' is common (dir foo/); strip for locate. */
                {
                    LONG ln;

                    ln = lhh_cstr_len(locname);
                    while (ln > 0 && locname[ln - 1] == '/') {
                        locname[ln - 1] = '\0';
                        ln--;
                    }
                }

                /*
                 * Virtual LHA: paths never contain ':'.  dos may pass
                 * "AmigaZen:" when the user typed LHA:AmigaZen: - reject.
                 */
                if (lhh_cstr_has(locname, ':')
                    && !lhh_is_volume_root_name(gd, locname)) {
                    DB1("locate reject colon name=%s\n", locname);
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
                    break;
                }

                /*
                 * root-handler: dp_Arg1==0 always yields a volume lock.
                 * We also accept that for empty / self names when parent is
                 * our LHA: root.  Non-empty names resolve into real volumes.
                 */
                err = ERROR_NO_FREE_STORE;
                lock = NULL;
                if (Pkt->dp_Arg1 == ZERO
                    && lhh_is_volume_root_name(gd, locname)) {
                    lock = lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL,
                        ZERO, NULL, NULL, mode, &err);
                } else if ((parent == NULL || parent->type == LHH_LOCK_ROOT)
                    && lhh_is_volume_root_name(gd, locname)) {
                    lock = lhh_lock_alloc(gd, LHH_LOCK_ROOT, NULL, NULL,
                        ZERO, NULL, NULL, mode, &err);
                } else {
                    err = ERROR_OBJECT_NOT_FOUND;
                    /*
                     * Pass stripped C name via a temporary BSTR in locname's
                     * buffer layout: resolve reads BSTR, so rebuild length.
                     */
                    {
                        static UBYTE locbstr[LHH_PATH_LEN + 1];
                        LONG ln;
                        LONG i;

                        ln = lhh_cstr_len(locname);
                        if (ln > 255) {
                            ln = 255;
                        }
                        locbstr[0] = (UBYTE)ln;
                        for (i = 0; i < ln; i++) {
                            locbstr[i + 1] = (UBYTE)locname[i];
                        }
                        lock = lhh_lock_resolve(gd, parent, (STRPTR)locbstr,
                            mode, &err);
                    }
                }
                if (lock != NULL) {
                    DB2("locate ok type=%s virt=%s\n",
                        lhh_lock_type_name(lock->type), lock->virt_path);
                    ReplyDosPacket1(gd, Pkt, MKBADDR(lock));
                } else {
                    DB2("locate fail name=%s err=%ld\n", locname, err);
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
                }
            }
            break;

        case ACTION_FREE_LOCK:
            {
                struct LhHLock *lock;

                lock = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                if (lock != NULL) {
                    lhh_lock_free(gd, lock);
                }
                ReplyDosPacket1(gd, Pkt, DOSTRUE);
            }
            break;

        case ACTION_COPY_DIR:
            {
                struct LhHLock *src;
                struct LhHLock *dup;

                src = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                err = ERROR_OBJECT_NOT_FOUND;
                dup = lhh_lock_dup(gd, src, &err);
                if (dup != NULL) {
                    ReplyDosPacket1(gd, Pkt, MKBADDR(dup));
                } else {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
                }
            }
            break;

        case ACTION_PARENT:
            {
                struct LhHLock *lock;
                struct LhHLock *parent;

                lock = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                err = ERROR_NO_FREE_STORE;
                parent = lhh_lock_parent(gd, lock, &err);
                if (parent != NULL) {
                    ReplyDosPacket1(gd, Pkt, MKBADDR(parent));
                } else {
                    if (lock == NULL || lock->type == LHH_LOCK_ROOT) {
                        err = ERROR_OBJECT_NOT_FOUND;
                    }
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
                }
            }
            break;

        case ACTION_EXAMINE_OBJECT:
            {
                struct LhHLock *lock;
                struct FileInfoBlock *fib;
                LONG ok;

                fib = (struct FileInfoBlock *)BADDR(Pkt->dp_Arg2);
                if (fib == NULL) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
                    break;
                }
                /*
                 * root-handler ignores the lock and always fills root fib.
                 * dos may send Arg1==0 when examining the assign itself.
                 */
                if (Pkt->dp_Arg1 == ZERO) {
                    examine_root_fib(fib);
                    ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    break;
                }
                lock = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                if (lock == NULL || lock->type == LHH_LOCK_ROOT) {
                    examine_root_fib(fib);
                    ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    break;
                }
                if (lock->type == LHH_LOCK_REAL) {
                    ok = lhh_host_examine(lock->real_lock, fib);
                    if (ok) {
                        lhh_examine_fib_name(fib, lock->virt_path);
                        ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE, IoErr());
                    }
                    break;
                }
                if (lock->type == LHH_LOCK_ARCHIVE) {
                    examine_archive_fib(lock, fib);
                    ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    break;
                }
                ok = LhExamine(lock->lh_lock, fib);
                if (ok) {
                    lhh_examine_fib_name(fib, lock->virt_path);
                    ReplyDosPacket1(gd, Pkt, DOSTRUE);
                } else {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE,
                        LhErr() ? LhErr() : ERROR_OBJECT_NOT_FOUND);
                }
            }
            break;

        case ACTION_EXAMINE_NEXT:
            {
                struct LhHLock *lock;
                struct FileInfoBlock *fib;
                struct DosList *dl;
                STRPTR str;
                LONG ok;

                lock = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                fib = (struct FileInfoBlock *)BADDR(Pkt->dp_Arg2);
                if (fib == NULL) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
                    break;
                }

                /* Root: volumes/assigns (no trailing ':', skip self). */
                if (lock == NULL || lock->type == LHH_LOCK_ROOT) {
                    dl = LockDosList(LDF_VOLUMES | LDF_ASSIGNS | LDF_READ);
                    if (dl != NULL) {
                        if (fib->fib_DiskKey != 0) {
                            struct DosList *sdl;

                            sdl = (struct DosList *)fib->fib_DiskKey;
                            while ((dl = NextDosEntry(dl,
                                    LDF_VOLUMES | LDF_ASSIGNS | LDF_READ))
                                != NULL) {
                                if (dl == sdl) {
                                    dl = NextDosEntry(dl,
                                        LDF_VOLUMES | LDF_ASSIGNS | LDF_READ);
                                    break;
                                }
                            }
                        } else {
                            dl = NextDosEntry(dl,
                                LDF_VOLUMES | LDF_ASSIGNS | LDF_READ);
                        }
                        while (dl != NULL) {
                            str = (STRPTR)BADDR(dl->dol_Name);
                            if (str != NULL && !IsSelfDosEntry(gd, str)) {
                                LONG n;

                                /* Bare volume name under LHA: - no trailing ':' */
                                n = (LONG)((UBYTE *)str)[0];
                                if (n > 106) {
                                    n = 106;
                                }
                                fib->fib_FileName[0] = (UBYTE)n;
                                strncpy(&fib->fib_FileName[1], &str[1],
                                    (size_t)n);
                                fib->fib_FileName[n + 1] = '\0';
                                fib->fib_DiskKey = (LONG)dl;
                                fib->fib_DirEntryType = ST_USERDIR;
                                fib->fib_EntryType = ST_USERDIR;
                                break;
                            }
                            dl = NextDosEntry(dl,
                                LDF_VOLUMES | LDF_ASSIGNS | LDF_READ);
                        }
                    }
                    UnLockDosList(LDF_VOLUMES | LDF_ASSIGNS);
                    if (dl == NULL) {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE,
                            ERROR_NO_MORE_ENTRIES);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    }
                    break;
                }

                if (lock->type == LHH_LOCK_REAL) {
                    ok = lhh_host_exnext(lock->real_lock, fib);
                    if (ok) {
                        ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE, IoErr());
                    }
                    break;
                }

                if (lock->type != LHH_LOCK_ARCHIVE) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
                    break;
                }
                if (fib->fib_DiskKey == 0) {
                    ok = LhExamine(lock->lh_lock, fib);
                    if (!ok) {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE,
                            ERROR_NO_MORE_ENTRIES);
                        break;
                    }
                    fib->fib_DiskKey = 1;
                    ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    break;
                }
                ok = LhExNext(lock->lh_lock, fib);
                if (ok) {
                    fib->fib_DiskKey = 1;
                    ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                } else {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_NO_MORE_ENTRIES);
                }
            }
            break;

        case ACTION_EXAMINE_ALL:
            {
                struct LhHLock *lock;
                struct ExAllControl *ec;
                LONG ok;

                lock = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                ec = (struct ExAllControl *)Pkt->dp_Arg5;

                if (lock == NULL || lock->type == LHH_LOCK_ROOT) {
                    /* Fall through to volume ExAll (root-handler body). */
                    struct ExAllData *ed;
                    struct ExAllData *last;
                    struct DosList *dl;
                    struct DosList *ldl;
                    struct DosList *sdl;
                    ULONG size;
                    ULONG type;
                    STRPTR str;
                    LONG len;
                    LONG elen;
                    int locked;

                    ed = (struct ExAllData *)Pkt->dp_Arg2;
                    size = (ULONG)Pkt->dp_Arg3;
                    type = (ULONG)Pkt->dp_Arg4;
                    if (ec != NULL) {
                        ec->eac_Entries = 0;
                    }
                    sdl = NULL;
                    last = NULL;
                    locked = 0;
                    if (ec != NULL && type > 0 && type < sizeof(sizes)
                        && (ldl = LockDosList(LDF_VOLUMES | LDF_ASSIGNS
                            | LDF_READ)) != NULL) {
                        locked = 1;
                        if (ec->eac_LastKey != 0) {
                            sdl = (struct DosList *)ec->eac_LastKey;
                            while ((dl = NextDosEntry(ldl,
                                    LDF_VOLUMES | LDF_ASSIGNS | LDF_READ))
                                != NULL) {
                                ldl = dl;
                                if (dl == sdl) {
                                    sdl = NULL;
                                    break;
                                }
                            }
                            ec->eac_LastKey = 0;
                        }
                        if (sdl == NULL && ldl != NULL) {
                            while ((dl = NextDosEntry(ldl,
                                    LDF_VOLUMES | LDF_ASSIGNS | LDF_READ))
                                != NULL) {
                                str = (STRPTR)BADDR(dl->dol_Name);
                                if (str == NULL || IsSelfDosEntry(gd, str)) {
                                    ldl = dl;
                                    continue;
                                }
                                elen = (LONG)sizes[type];
                                /* +1 for NUL; bare name, no trailing ':' */
                                len = (LONG)(((str[0] + 1) + 3) & ~(3));
                                elen += len;
                                if (size > (ULONG)elen) {
                                    size -= (ULONG)elen;
                                    switch (type) {
                                    case ED_OWNER:
                                        ed->ed_OwnerUID = 0;
                                        ed->ed_OwnerGID = 0;
                                    case ED_COMMENT:
                                        ed->ed_Comment = NULL;
                                    case ED_DATE:
                                        ed->ed_Days = 0;
                                        ed->ed_Mins = 0;
                                        ed->ed_Ticks = 0;
                                    case ED_PROTECTION:
                                        ed->ed_Prot = 0;
                                    case ED_SIZE:
                                        ed->ed_Size = 0;
                                    case ED_TYPE:
                                        ed->ed_Type = ST_USERDIR;
                                    case ED_NAME:
                                        ed->ed_Name = (STRPTR)(ed + 1);
                                        break;
                                    }
                                    strncpy(ed->ed_Name, &str[1],
                                        (size_t)str[0]);
                                    ed->ed_Name[str[0]] = '\0';
                                    last = ed;
                                    ed = (struct ExAllData *)(((STRPTR)ed)
                                        + elen);
                                    last->ed_Next = ed;
                                    ec->eac_Entries++;
                                } else {
                                    ec->eac_LastKey = (ULONG)ldl;
                                    break;
                                }
                                ldl = dl;
                            }
                        }
                        if (last != NULL) {
                            last->ed_Next = NULL;
                        }
                    }
                    if (locked) {
                        UnLockDosList(LDF_VOLUMES | LDF_ASSIGNS);
                    }
                    if (ec == NULL || ec->eac_LastKey == 0) {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE,
                            ERROR_NO_MORE_ENTRIES);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    }
                    break;
                }

                if (lock->type == LHH_LOCK_REAL) {
                    ok = lhh_host_exall(lock->real_lock,
                        (STRPTR)Pkt->dp_Arg2, Pkt->dp_Arg3, Pkt->dp_Arg4, ec);
                    if (ok) {
                        ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    } else {
                        err = IoErr();
                        if (err == 0) {
                            err = ERROR_NO_MORE_ENTRIES;
                        }
                        ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
                    }
                    break;
                }
                if (lock->type == LHH_LOCK_ARCHIVE && lock->lh_lock != ZERO) {
                    ok = LhExAll(lock->lh_lock, (STRPTR)Pkt->dp_Arg2,
                        Pkt->dp_Arg3, Pkt->dp_Arg4, ec);
                    if (ok) {
                        ReplyDosPacket2(gd, Pkt, DOSTRUE, 0);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE,
                            LhErr() ? LhErr() : ERROR_NO_MORE_ENTRIES);
                    }
                    break;
                }
                ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_OBJECT_WRONG_TYPE);
            }
            break;

        case ACTION_EXAMINE_ALL_END:
            ReplyDosPacket1(gd, Pkt, DOSTRUE);
            break;

        case ACTION_FINDINPUT:
        case ACTION_FINDOUTPUT:
        case ACTION_FINDUPDATE:
            {
                struct FileHandle *fh;
                struct LhHLock *parent;
                struct LhHHandle *hh;
                LONG mode;

                fh = (struct FileHandle *)BADDR(Pkt->dp_Arg1);
                parent = pkt_lock(gd, (BPTR)Pkt->dp_Arg2);
                if (Pkt->dp_Type == ACTION_FINDOUTPUT) {
                    mode = MODE_NEWFILE;
                } else if (Pkt->dp_Type == ACTION_FINDUPDATE) {
                    mode = MODE_READWRITE;
                } else {
                    mode = MODE_OLDFILE;
                }
                err = ERROR_OBJECT_NOT_FOUND;
                hh = lhh_handle_open(gd, parent,
                    Pkt->dp_Arg3 ? (STRPTR)BADDR(Pkt->dp_Arg3) : NULL,
                    mode, &err);
                if (hh == NULL) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
                    break;
                }
                fh->fh_Type = gd->gd_Port;
                fh->fh_Port = NULL;
                fh->fh_Arg1 = (LONG)hh;
                ReplyDosPacket1(gd, Pkt, DOSTRUE);
            }
            break;

        case ACTION_READ:
            {
                struct LhHHandle *hh;
                LONG n;

                hh = (struct LhHHandle *)Pkt->dp_Arg1;
                n = lhh_handle_read(gd, hh, (APTR)Pkt->dp_Arg2, Pkt->dp_Arg3);
                if (n < 0) {
                    ReplyDosPacket2(gd, Pkt, -1, ERROR_OBJECT_WRONG_TYPE);
                } else {
                    ReplyDosPacket1(gd, Pkt, n);
                }
            }
            break;

        case ACTION_WRITE:
            {
                struct LhHHandle *hh;
                LONG n;

                hh = (struct LhHHandle *)Pkt->dp_Arg1;
                n = lhh_handle_write(gd, hh, (APTR)Pkt->dp_Arg2, Pkt->dp_Arg3);
                if (n < 0) {
                    ReplyDosPacket2(gd, Pkt, -1, ERROR_WRITE_PROTECTED);
                } else {
                    ReplyDosPacket1(gd, Pkt, n);
                }
            }
            break;

        case ACTION_SEEK:
            {
                struct LhHHandle *hh;
                LONG n;

                hh = (struct LhHHandle *)Pkt->dp_Arg1;
                n = lhh_handle_seek(gd, hh, Pkt->dp_Arg2, Pkt->dp_Arg3);
                if (n < 0) {
                    ReplyDosPacket2(gd, Pkt, -1, ERROR_SEEK_ERROR);
                } else {
                    ReplyDosPacket1(gd, Pkt, n);
                }
            }
            break;

        case ACTION_END:
            lhh_handle_close(gd, (struct LhHHandle *)Pkt->dp_Arg1);
            ReplyDosPacket1(gd, Pkt, DOSTRUE);
            break;

        case ACTION_DELETE_OBJECT:
            {
                struct LhHLock *parent;
                LONG ok;

                parent = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                err = ERROR_OBJECT_NOT_FOUND;
                ok = lhh_delete_name(gd, parent,
                    Pkt->dp_Arg2 ? (STRPTR)BADDR(Pkt->dp_Arg2) : NULL, &err);
                if (ok) {
                    ReplyDosPacket1(gd, Pkt, DOSTRUE);
                } else {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, err);
                }
            }
            break;

        case ACTION_INFO:
            {
                struct LhHLock *lock;
                struct InfoData *info;
                LONG ok;

                lock = pkt_lock(gd, (BPTR)Pkt->dp_Arg1);
                info = (struct InfoData *)BADDR(Pkt->dp_Arg2);
                if (info == NULL) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_NO_FREE_STORE);
                    break;
                }
                if (lock == NULL || lock->type == LHH_LOCK_ROOT) {
                    info->id_NumSoftErrors = 0;
                    info->id_UnitNumber = 0;
                    info->id_DiskState = ID_VALIDATED;
                    info->id_NumBlocks = 1;
                    info->id_NumBlocksUsed = 1;
                    info->id_BytesPerBlock = 512;
                    info->id_DiskType = ID_DOS_DISK;
                    info->id_VolumeNode = MKBADDR(gd->gd_DosList);
                    info->id_InUse = (gd->gd_UsageCnt > 0) ? DOSTRUE : DOSFALSE;
                    ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    break;
                }
                if (lock->type == LHH_LOCK_REAL) {
                    ok = lhh_host_info(lock->real_lock, info);
                    if (ok) {
                        info->id_VolumeNode = MKBADDR(gd->gd_DosList);
                        ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE, IoErr());
                    }
                    break;
                }
                if (lock->lh_lock != ZERO) {
                    ok = LhInfo(lock->lh_lock, info);
                    if (ok) {
                        info->id_VolumeNode = MKBADDR(gd->gd_DosList);
                        ReplyDosPacket1(gd, Pkt, DOSTRUE);
                    } else {
                        ReplyDosPacket2(gd, Pkt, DOSFALSE,
                            LhErr() ? LhErr() : ERROR_OBJECT_NOT_FOUND);
                    }
                    break;
                }
                ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
            }
            break;

        case ACTION_DISK_INFO:
            {
                struct InfoData *info;

                info = (struct InfoData *)BADDR(Pkt->dp_Arg1);
                if (info == NULL) {
                    ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_NO_FREE_STORE);
                    break;
                }
                info->id_NumSoftErrors = 0;
                info->id_UnitNumber = 0;
                info->id_DiskState = ID_VALIDATED;
                info->id_NumBlocks = 1;
                info->id_NumBlocksUsed = 1;
                info->id_BytesPerBlock = 512;
                info->id_DiskType = ID_DOS_DISK;
                info->id_VolumeNode = MKBADDR(gd->gd_DosList);
                info->id_InUse = (gd->gd_UsageCnt > 0) ? DOSTRUE : DOSFALSE;
                ReplyDosPacket1(gd, Pkt, DOSTRUE);
            }
            break;

        case ACTION_CURRENT_VOLUME:
            ReplyDosPacket1(gd, Pkt, MKBADDR(gd->gd_DosList));
            break;

        default:
            DB2("unknown pkt %s (%ld)\n",
                lhh_pkt_name(Pkt->dp_Type), Pkt->dp_Type);
            ReplyDosPacket2(gd, Pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
            break;
        }
        lhh_db_pkt_leave();
    }

    if (gd->gd_DosList != NULL) {
        gd->gd_DosList->dol_Task = NULL;
    }
    DB2("shutdown usage=%ld locks=%ld\n",
        gd->gd_UsageCnt, gd->gd_LockCnt);
    lhh_log_close();
    closeres(gd);
    return RETURN_OK;
}
