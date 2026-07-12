/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Publish a DLT_VOLUME so Workbench can show a disk icon.
 *
 * Device node from Mount is DLT_DEVICE "LHA" (paths use LHA:).
 * WB backdrop icons come from DLT_VOLUME entries (name shown on icon),
 * same split as RAM: vs "Ram Disk".
 *
 * HappyENV ICON mode: splice the volume at the head of di_DevInfo
 * (dos list).  AddDosEntry alone often never reaches WB for soft FS.
 */

#include "lh-handler.h"

#include <devices/input.h>
#include <devices/inputevent.h>
#include <proto/alib.h>

#ifdef __SASC
#undef SysBase
#undef DOSBase
#undef UtilityBase
#define gd LhhGD
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#define UtilityBase gd->gd_UtilityBase
#endif

#ifndef DLT_VOLUME
#define DLT_VOLUME 2
#endif

#ifndef ZERO
#define ZERO 0
#endif

/*
 * Volume label on the WB icon.  Must differ from device name "LHA" when
 * using AddDosEntry; HappyENV-style splice can use either.
 */
#ifndef LHH_VOLUME_NAME
#define LHH_VOLUME_NAME "Lha"
#endif

static int lhh_vol_ch_eq_i(char a, char b)
{
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
    }
    return a == b;
}

/* DosBase -> RootNode -> DosInfo -> di_DevInfo (BPTR list head). */
static BPTR *lhh_doslist_head_ptr(void)
{
    struct DosLibrary *dos;
    struct RootNode *root;
    struct DosInfo *info;

    dos = (struct DosLibrary *)DOSBase;
    if (dos == NULL) {
        return NULL;
    }
    root = (struct RootNode *)dos->dl_Root;
    if (root == NULL) {
        return NULL;
    }
    info = (struct DosInfo *)BADDR(root->rn_Info);
    if (info == NULL) {
        return NULL;
    }
    return &info->di_DevInfo;
}

static void lhh_volume_fill(GD gdx, struct DosList *vol)
{
    if (gdx == NULL || vol == NULL) {
        return;
    }
    vol->dol_Type = DLT_VOLUME;
    vol->dol_Task = gdx->gd_Port;
    vol->dol_Lock = ZERO;
    DateStamp(&vol->dol_misc.dol_volume.dol_VolumeDate);
    vol->dol_misc.dol_volume.dol_LockList = ZERO;
    /* Same as HappyENV / FFS-style 'DOS\\0' id. */
    vol->dol_misc.dol_volume.dol_DiskType = ID_DOS_DISK;
}

static void lhh_volume_diskchange(void)
{
    struct MsgPort *port;
    struct IOStdReq *req;
    struct InputEvent ie;
    LONG err;

    if (LhhGD == NULL) {
        return;
    }
    port = CreateMsgPort();
    if (port == NULL) {
        return;
    }
    req = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (req == NULL) {
        DeleteMsgPort(port);
        return;
    }
    err = OpenDevice((STRPTR)"input.device", 0, (struct IORequest *)req, 0);
    if (err == 0) {
        ie.ie_NextEvent = NULL;
        ie.ie_Class = IECLASS_DISKREMOVED;
        ie.ie_SubClass = 0;
        ie.ie_Code = 0;
        ie.ie_Qualifier = 0;
        ie.ie_EventAddress = NULL;
        req->io_Command = IND_WRITEEVENT;
        req->io_Data = (APTR)&ie;
        req->io_Length = sizeof(ie);
        DoIO((struct IORequest *)req);

        ie.ie_Class = IECLASS_DISKINSERTED;
        DoIO((struct IORequest *)req);

        CloseDevice((struct IORequest *)req);
        DB("volume: diskchange poke\n");
    }
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
}

/*
 * HappyENV ICON: vol->dol_Next = old head; di_DevInfo = volume.
 * Forbid so we do not race other list walkers.
 */
static int lhh_volume_splice_in(GD gdx)
{
    BPTR *head;
    BPTR old;

    head = lhh_doslist_head_ptr();
    if (head == NULL || gdx->gd_Volume == NULL) {
        return 0;
    }
    Forbid();
    old = *head;
    gdx->gd_Volume->dol_Next = old;
    *head = MKBADDR(gdx->gd_Volume);
    Permit();
    DB1("volume: spliced into DosList head name=%s\n", LHH_VOLUME_NAME);
    return 1;
}

static int lhh_volume_splice_out(GD gdx)
{
    BPTR *head;
    BPTR cur;
    BPTR mine;
    struct DosList *dl;
    struct DosList *prev;

    head = lhh_doslist_head_ptr();
    if (head == NULL || gdx->gd_Volume == NULL) {
        return 0;
    }
    mine = MKBADDR(gdx->gd_Volume);
    Forbid();
    if (*head == mine) {
        *head = gdx->gd_Volume->dol_Next;
        gdx->gd_Volume->dol_Next = ZERO;
        Permit();
        return 1;
    }
    prev = NULL;
    cur = *head;
    while (cur != ZERO) {
        dl = (struct DosList *)BADDR(cur);
        if (dl == NULL) {
            break;
        }
        if (dl->dol_Next == mine) {
            dl->dol_Next = gdx->gd_Volume->dol_Next;
            gdx->gd_Volume->dol_Next = ZERO;
            Permit();
            return 1;
        }
        prev = dl;
        cur = dl->dol_Next;
        (void)prev;
    }
    Permit();
    return 0;
}

int lhh_volume_try_add(GD gdx)
{
    ULONG flags;

    if (gdx == NULL || gdx->gd_Volume == NULL) {
        return 0;
    }
    if (gdx->gd_VolumeAdded) {
        return 1;
    }

    /*
     * Prefer HappyENV-style splice (always visible to VolumeListSearch).
     * Fall back to AddDosEntry if splice cannot find DosInfo.
     */
    if (lhh_volume_splice_in(gdx)) {
        gdx->gd_VolumeAdded = 1;
        gdx->gd_VolPending = 0;
        lhh_volume_diskchange();
        return 1;
    }

    flags = LDF_VOLUMES | LDF_WRITE;
    if (lhh_attempt_lock_doslist(gdx, flags) == NULL) {
        DB("volume: DosList busy, will retry\n");
        gdx->gd_VolPending = 1;
        return 0;
    }
    if (AddDosEntry(gdx->gd_Volume)) {
        UnLockDosList(flags);
        gdx->gd_VolumeAdded = 1;
        gdx->gd_VolPending = 0;
        DB1("volume: AddDosEntry ok name=%s\n", LHH_VOLUME_NAME);
        lhh_volume_diskchange();
        return 1;
    }
    UnLockDosList(flags);
    DB1("volume: AddDosEntry fail err=%ld\n", (LONG)IoErr());
    gdx->gd_VolPending = 0;
    return 0;
}

void lhh_volume_init(GD gdx)
{
    struct DosList *vol;

    if (gdx == NULL) {
        return;
    }
    gdx->gd_Volume = NULL;
    gdx->gd_VolumeAdded = 0;
    gdx->gd_VolPending = 0;

    vol = MakeDosEntry((STRPTR)LHH_VOLUME_NAME, DLT_VOLUME);
    if (vol == NULL) {
        DB("volume: MakeDosEntry fail\n");
        return;
    }
    lhh_volume_fill(gdx, vol);
    gdx->gd_Volume = vol;
    DB1("volume: created label='%s' (device stays LHA:)\n", LHH_VOLUME_NAME);

    if (!lhh_volume_try_add(gdx)) {
        gdx->gd_VolPending = 1;
    }
}

void lhh_volume_remove(GD gdx)
{
    ULONG flags;

    if (gdx == NULL || gdx->gd_Volume == NULL) {
        return;
    }
    if (gdx->gd_VolumeAdded) {
        if (lhh_volume_splice_out(gdx)) {
            lhh_volume_diskchange();
            FreeDosEntry(gdx->gd_Volume);
            DB("volume: spliced out\n");
        } else {
            flags = LDF_VOLUMES | LDF_WRITE;
            if (lhh_attempt_lock_doslist(gdx, flags) != NULL) {
                RemDosEntry(gdx->gd_Volume);
                UnLockDosList(flags);
                lhh_volume_diskchange();
                FreeDosEntry(gdx->gd_Volume);
                DB("volume: RemDosEntry\n");
            } else {
                DB("volume: remove skipped (busy), leaking node\n");
                gdx->gd_Volume = NULL;
                gdx->gd_VolumeAdded = 0;
                gdx->gd_VolPending = 0;
                return;
            }
        }
        gdx->gd_VolumeAdded = 0;
    } else {
        FreeDosEntry(gdx->gd_Volume);
    }
    gdx->gd_Volume = NULL;
    gdx->gd_VolPending = 0;
}

BPTR lhh_volume_bptr(GD gdx)
{
    if (gdx == NULL) {
        return ZERO;
    }
    if (gdx->gd_Volume != NULL) {
        return MKBADDR(gdx->gd_Volume);
    }
    if (gdx->gd_DosList != NULL) {
        return MKBADDR(gdx->gd_DosList);
    }
    return ZERO;
}

int lhh_is_self_volume_entry(GD gdx, STRPTR bname)
{
    STRPTR self;
    LONG n;
    LONG i;
    const char *vname;

    if (bname == NULL) {
        return 0;
    }
    if (gdx != NULL && gdx->gd_DosList != NULL) {
        self = (STRPTR)BADDR(gdx->gd_DosList->dol_Name);
        if (self != NULL) {
            n = (LONG)((UBYTE *)self)[0];
            if (n == (LONG)((UBYTE *)bname)[0]) {
                for (i = 0; i < n; i++) {
                    if (!lhh_vol_ch_eq_i(self[i + 1], bname[i + 1])) {
                        break;
                    }
                }
                if (i == n) {
                    return 1;
                }
            }
        }
    }
    vname = LHH_VOLUME_NAME;
    n = lhh_cstr_len(vname);
    if (n == (LONG)((UBYTE *)bname)[0]) {
        for (i = 0; i < n; i++) {
            if (!lhh_vol_ch_eq_i(vname[i], bname[i + 1])) {
                break;
            }
        }
        if (i == n) {
            return 1;
        }
    }
    return 0;
}
