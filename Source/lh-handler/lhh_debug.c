/*
 * lhh_debug.c - File log via a child process.
 *
 * The handler process must not call dos.library Open/Write (those wait on
 * pr_MsgPort, which is also our packet port).  A small child process owns
 * the log file and uses normal Open/Write on its own pr_MsgPort.
 *
 * Parent sends log lines as messages to a *private* MsgPort on the child.
 * The child's pr_MsgPort is reserved for dos Open/Write/Close replies only
 * (mixing custom msgs there causes AN_AsyncPkt / "Unexpected packet received").
 *
 * Startup must Wait() (not busy-spin): the handler is Priority 5, so a
 * lower-priority child never runs while we spin, leaving an empty log.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>

#define __USE_SYSBASE 42
#include <proto/exec.h>
#include <proto/dos.h>

#include "lhh_debug.h"
#include "lh-handler.h"

#ifdef LHH_DEBUG

#ifdef __SASC
#undef SysBase
#undef DOSBase
#define gd LhhGD
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#endif

#ifndef ZERO
#define ZERO 0
#endif

#define LHH_LOCK_ROOT     0
#define LHH_LOCK_REAL     1
#define LHH_LOCK_ARCHIVE  2
#define LHH_LOCK_ENTRY    3
#define LHH_LOCK_VINFO    4

#define LHH_DBG_BUFSIZE   512
#define LHH_LOG_RING_SIZE 8192
#define LHH_LOG_MSG_MAX   480

#ifndef PROJECTNAME
#define PROJECTNAME "lh-handler"
#endif

/* Message from handler to log child. */
struct LhhLogMsg {
    struct Message msg;
    LONG len;
    char text[LHH_LOG_MSG_MAX];
};

#define LHH_LOG_CMD_QUIT  (-1)

/*
 * Shared handshake block in public memory so parent/child do not depend on
 * A4 for the ready signal (SMALLDATA).  Port pointer still lives here.
 */
struct LhhLogShared {
    struct Process *parent;
    BYTE parentsig;
    struct MsgPort *childport;
    LONG status; /* 1=file ok, 0=no file but port up, -1=failed */
    char path[128];
};

struct LhhPktName {
    LONG type;
    const char *name;
};

static const struct LhhPktName lhh_pkt_names[] = {
    /* Numbers from RKRM dos.library packet docs */
    { 5,    "DIE" },
    { 7,    "CURRENT_VOLUME" },
    { 8,    "LOCATE_OBJECT" },
    { 9,    "RENAME_DISK" },
    { 15,   "FREE_LOCK" },
    { 16,   "DELETE_OBJECT" },
    { 17,   "RENAME_OBJECT" },
    { 18,   "MORE_CACHE" },
    { 19,   "COPY_DIR" },
    { 21,   "SET_PROTECT" },
    { 22,   "CREATE_DIR" },
    { 23,   "EXAMINE_OBJECT" },
    { 24,   "EXAMINE_NEXT" },
    { 25,   "DISK_INFO" },
    { 26,   "INFO" },
    { 27,   "FLUSH" },
    { 28,   "SET_COMMENT" },
    { 29,   "PARENT" },
    { 31,   "INHIBIT" },
    { 34,   "SET_DATE" },
    { 40,   "SAME_LOCK" },
    { 82,   "READ" },
    { 87,   "WRITE" },
    { 1004, "FINDUPDATE" },
    { 1005, "FINDINPUT" },
    { 1006, "FINDOUTPUT" },
    { 1007, "END" },
    { 1008, "SEEK" },
    { 1020, "FORMAT" },
    { 1021, "MAKE_LINK" },
    { 1022, "SET_FILE_SIZE" },
    { 1023, "WRITE_PROTECT" },
    { 1024, "READ_LINK" },
    { 1026, "FH_FROM_LOCK" },
    { 1027, "IS_FILESYSTEM" },
    { 1028, "CHANGE_MODE" },
    { 1030, "COPY_DIR_FH" },
    { 1031, "PARENT_FH" },
    { 1033, "EXAMINE_ALL" },
    { 1034, "EXAMINE_FH" },
    { 1035, "EXAMINE_ALL_END" },
    { 1036, "SET_OWNER" },
    { 2008, "LOCK_RECORD" },
    { 2009, "FREE_RECORD" },
    { 4097, "ADD_NOTIFY" },
    { 4098, "REMOVE_NOTIFY" },
    { 4200, "SERIALIZE_DISK" },
    { 0,    NULL }
};

static char lhh_log_ring[LHH_LOG_RING_SIZE];
static LONG lhh_log_ring_used;
static LONG lhh_pkt_depth;
static int lhh_log_lazy_done;
static char lhh_log_pathbuf[128];

static struct LhhLogShared *lhh_log_shared;
static struct Process *lhh_log_child;
static int lhh_log_ok;

static void lhh_log_ring_append(const char *text);
static void lhh_log_flush_ring(void);
static void lhh_log_send(const char *text, LONG len);

static LONG lhh_str_len(const char *s)
{
    LONG n;

    n = 0;
    if (s != NULL) {
        while (s[n] != '\0') {
            n++;
        }
    }
    return n;
}

static void lhh_prefix(char *buf, LONG buflen, LONG *used)
{
    LONG i;
    LONG n;

    n = 0;
    for (i = 0; PROJECTNAME[i] != '\0' && n < buflen - 16; i++) {
        buf[n++] = PROJECTNAME[i];
    }
    if (n + 2 < buflen) {
        buf[n++] = ':';
        buf[n++] = ' ';
    }
    buf[n] = '\0';
    if (used != NULL) {
        *used = n;
    }
}

#ifdef __SASC
static void __asm lhh_putch(register __d0 char c, register __a3 char **p)
{
    if (p != NULL && *p != NULL) {
        *(*p)++ = c;
    }
}
#else
static void lhh_putch(char c, char **p)
{
    if (p != NULL && *p != NULL) {
        *(*p)++ = c;
    }
}
#endif

/*
 * Log child: normal dos Open/Write on pr_MsgPort; log lines arrive on a
 * *private* MsgPort.  Never PutMsg log traffic to pr_MsgPort - dos.library
 * WaitPkt for Write/Open would see it and Alert AN_AsyncPkt
 * ("Unexpected packet received").
 */
#ifdef __SASC
static void __saveds lhh_log_child_entry(void)
#else
static void lhh_log_child_entry(void)
#endif
{
    struct Library *AbsSysBase;
    struct DosLibrary *AbsDOSBase;
    struct MsgPort *logport;
    struct LhhLogShared *shared;
    BPTR fh;
    struct LhhLogMsg *lm;
    int running;
    const char *used;
    LONG j;

#ifdef __SASC
#undef SysBase
#undef DOSBase
#define SysBase AbsSysBase
#define DOSBase AbsDOSBase
#endif

    AbsSysBase = *(struct Library **)4L;
    AbsDOSBase = NULL;
    logport = NULL;
    fh = ZERO;
    running = 1;
    used = NULL;
    shared = lhh_log_shared;

    AbsDOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (AbsDOSBase == NULL) {
        if (shared != NULL) {
            shared->status = -1;
            shared->childport = NULL;
            if (shared->parent != NULL && shared->parentsig >= 0) {
                Signal((struct Task *)shared->parent,
                    1UL << shared->parentsig);
            }
        }
#ifdef __SASC
#undef SysBase
#undef DOSBase
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#endif
        return;
    }

    logport = CreateMsgPort();
    if (logport == NULL) {
        if (shared != NULL) {
            shared->status = -1;
            shared->childport = NULL;
            if (shared->parent != NULL && shared->parentsig >= 0) {
                Signal((struct Task *)shared->parent,
                    1UL << shared->parentsig);
            }
        }
        CloseLibrary((struct Library *)AbsDOSBase);
#ifdef __SASC
#undef SysBase
#undef DOSBase
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#endif
        return;
    }

    fh = Open((STRPTR)LHH_LOG_PATH, MODE_NEWFILE);
    if (fh != ZERO) {
        used = LHH_LOG_PATH;
    } else {
        fh = Open((STRPTR)LHH_LOG_FALLBACK, MODE_NEWFILE);
        if (fh != ZERO) {
            used = LHH_LOG_FALLBACK;
        }
    }

    if (shared != NULL) {
        if (used != NULL) {
            for (j = 0; used[j] != '\0' && j < (LONG)sizeof(shared->path) - 1;
                j++) {
                shared->path[j] = used[j];
            }
            shared->path[j] = '\0';
            shared->status = 1;
        } else {
            shared->path[0] = '\0';
            shared->status = 0;
        }
        shared->childport = logport;
        if (shared->parent != NULL && shared->parentsig >= 0) {
            Signal((struct Task *)shared->parent, 1UL << shared->parentsig);
        }
    }

    while (running) {
        WaitPort(logport);
        while ((lm = (struct LhhLogMsg *)GetMsg(logport)) != NULL) {
            if (lm->len == LHH_LOG_CMD_QUIT) {
                running = 0;
            } else if (fh != ZERO && lm->len > 0) {
                Write(fh, lm->text, lm->len);
            }
            FreeMem(lm, sizeof(struct LhhLogMsg));
        }
    }

    if (fh != ZERO) {
        Close(fh);
    }
    if (shared != NULL) {
        shared->childport = NULL;
    }
    DeleteMsgPort(logport);
    CloseLibrary((struct Library *)AbsDOSBase);

#ifdef __SASC
#undef SysBase
#undef DOSBase
#define SysBase     gd->gd_SysBase
#define DOSBase     gd->gd_DOSBase
#endif
}

static int lhh_log_start_child(void)
{
    struct TagItem tags[6];
    struct LhhLogShared *shared;
    LONG i;
    BYTE sig;

    if (lhh_log_ok && lhh_log_shared != NULL
        && lhh_log_shared->childport != NULL) {
        return 1;
    }
    if (LhhGD == NULL || DOSBase == NULL) {
        return 0;
    }

    sig = AllocSignal(-1);
    if (sig < 0) {
        return 0;
    }

    shared = (struct LhhLogShared *)AllocMem(sizeof(struct LhhLogShared),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (shared == NULL) {
        FreeSignal(sig);
        return 0;
    }
    shared->parent = (struct Process *)FindTask(NULL);
    shared->parentsig = sig;
    shared->childport = NULL;
    shared->status = -1;
    shared->path[0] = '\0';
    lhh_log_shared = shared;
    lhh_log_pathbuf[0] = '\0';

    i = 0;
    tags[i].ti_Tag = NP_Entry;
    tags[i].ti_Data = (ULONG)lhh_log_child_entry;
    i++;
    tags[i].ti_Tag = NP_StackSize;
    tags[i].ti_Data = 8192;
    i++;
    tags[i].ti_Tag = NP_Name;
    tags[i].ti_Data = (ULONG)"lh-log";
    i++;
    /* Above handler Priority 5 so Open runs while parent Wait()s. */
    tags[i].ti_Tag = NP_Priority;
    tags[i].ti_Data = (ULONG)6;
    i++;
    tags[i].ti_Tag = TAG_DONE;
    tags[i].ti_Data = 0;

    SetSignal(0, 1UL << sig);
    lhh_log_child = CreateNewProc(tags);
    if (lhh_log_child == NULL) {
        FreeMem(shared, sizeof(struct LhhLogShared));
        lhh_log_shared = NULL;
        FreeSignal(sig);
        return 0;
    }

    /* Yield until child Signals - busy-spin starves lower-priority tasks. */
    Wait(1UL << sig);

    FreeSignal(sig);
    shared->parentsig = -1;

    if (shared->childport == NULL) {
        FreeMem(shared, sizeof(struct LhhLogShared));
        lhh_log_shared = NULL;
        lhh_log_child = NULL;
        return 0;
    }

    if (shared->path[0] != '\0') {
        for (i = 0; shared->path[i] != '\0'
            && i < (LONG)sizeof(lhh_log_pathbuf) - 1; i++) {
            lhh_log_pathbuf[i] = shared->path[i];
        }
        lhh_log_pathbuf[i] = '\0';
    }

    /* Port is up: accept logging even if only RAM: or primary Open failed. */
    lhh_log_ok = 1;
    if (shared->status < 0) {
        lhh_log_ok = 0;
        return 0;
    }
    if (shared->status == 0) {
        /* No file - still useless for the user. */
        lhh_log_ok = 0;
        return 0;
    }
    return 1;
}

static void lhh_log_send(const char *text, LONG len)
{
    struct LhhLogMsg *lm;
    struct MsgPort *dest;
    LONG i;
    LONG n;

    if (lhh_log_shared == NULL) {
        return;
    }
    dest = lhh_log_shared->childport;
    if (dest == NULL || text == NULL || len <= 0) {
        return;
    }
    n = len;
    if (n > LHH_LOG_MSG_MAX - 1) {
        n = LHH_LOG_MSG_MAX - 1;
    }
    lm = (struct LhhLogMsg *)AllocMem(sizeof(struct LhhLogMsg),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (lm == NULL) {
        return;
    }
    lm->msg.mn_Node.ln_Type = NT_MESSAGE;
    lm->msg.mn_Length = (UWORD)sizeof(struct LhhLogMsg);
    lm->msg.mn_ReplyPort = NULL; /* fire-and-forget; child FreeMem's */
    lm->len = n;
    for (i = 0; i < n; i++) {
        lm->text[i] = text[i];
    }
    lm->text[n] = '\0';
    PutMsg(dest, &lm->msg);
}

void lhh_log_open(const char *path)
{
    (void)path;
    if (!lhh_log_start_child()) {
        lhh_log_ok = 0;
        return;
    }
    lhh_log_send("lh-handler: log opened\r\n", 26);
}

void lhh_log_lazy_init(void)
{
    if (lhh_log_lazy_done) {
        return;
    }
    lhh_log_lazy_done = 1;
    if (!lhh_log_ok) {
        lhh_log_open(LHH_LOG_PATH);
    }
    if (lhh_log_ok) {
        lhh_log_send("lh-handler: lazy log init\r\n", 27);
    }
}

const char *lhh_log_path_used(void)
{
    if (!lhh_log_ok || lhh_log_pathbuf[0] == '\0') {
        return NULL;
    }
    return lhh_log_pathbuf;
}

void lhh_log_close(void)
{
    struct LhhLogMsg *lm;
    struct MsgPort *dest;
    LONG i;

    lhh_log_flush_ring();
    if (lhh_log_ok) {
        lhh_log_send("lh-handler: log closed\r\n", 25);
    }
    dest = NULL;
    if (lhh_log_shared != NULL) {
        dest = lhh_log_shared->childport;
    }
    if (dest != NULL) {
        lm = (struct LhhLogMsg *)AllocMem(sizeof(struct LhhLogMsg),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (lm != NULL) {
            lm->msg.mn_Node.ln_Type = NT_MESSAGE;
            lm->msg.mn_Length = (UWORD)sizeof(struct LhhLogMsg);
            lm->len = LHH_LOG_CMD_QUIT;
            PutMsg(dest, &lm->msg);
        }
        for (i = 0; i < 200000 && lhh_log_shared != NULL
            && lhh_log_shared->childport != NULL; i++) {
            /* child clears childport on exit; brief spin is OK after Quit */
        }
    }
    if (lhh_log_shared != NULL) {
        FreeMem(lhh_log_shared, sizeof(struct LhhLogShared));
        lhh_log_shared = NULL;
    }
    lhh_log_ok = 0;
    lhh_log_child = NULL;
    lhh_log_ring_used = 0;
}

int lhh_log_active(void)
{
    return (lhh_log_ok && lhh_log_shared != NULL
        && lhh_log_shared->childport != NULL) ? 1 : 0;
}

void lhh_db_pkt_enter(void)
{
    lhh_pkt_depth++;
}

void lhh_db_pkt_leave(void)
{
    if (lhh_pkt_depth > 0) {
        lhh_pkt_depth--;
    }
    if (lhh_pkt_depth == 0) {
        lhh_log_lazy_init();
        lhh_log_flush_ring();
    }
}

static void lhh_log_ring_append(const char *text)
{
    LONG len;
    LONG i;
    LONG room;

    if (text == NULL) {
        return;
    }
    len = lhh_str_len(text);
    if (len <= 0) {
        return;
    }
    if (len >= LHH_LOG_RING_SIZE - 1) {
        len = LHH_LOG_RING_SIZE - 2;
    }
    room = LHH_LOG_RING_SIZE - 1 - lhh_log_ring_used;
    if (len > room) {
        LONG drop;
        LONG j;

        drop = len - room;
        if (drop >= lhh_log_ring_used) {
            lhh_log_ring_used = 0;
        } else {
            for (j = 0; j < lhh_log_ring_used - drop; j++) {
                lhh_log_ring[j] = lhh_log_ring[j + drop];
            }
            lhh_log_ring_used -= drop;
        }
    }
    for (i = 0; i < len; i++) {
        lhh_log_ring[lhh_log_ring_used++] = text[i];
    }
    lhh_log_ring[lhh_log_ring_used] = '\0';
}

static void lhh_log_flush_ring(void)
{
    if (!lhh_log_active() || lhh_log_ring_used <= 0) {
        lhh_log_ring_used = 0;
        return;
    }
    lhh_log_send(lhh_log_ring, lhh_log_ring_used);
    lhh_log_ring_used = 0;
    lhh_log_ring[0] = '\0';
}

static void lhh_log_emit(const char *text)
{
    LONG len;

    if (text == NULL) {
        return;
    }
    if (lhh_pkt_depth > 0) {
        lhh_log_ring_append(text);
        return;
    }
    if (!lhh_log_active()) {
        return;
    }
    len = lhh_str_len(text);
    lhh_log_send(text, len);
}

void lhh_db1(const char *fmt, ...)
{
    static char buf[LHH_DBG_BUFSIZE];
    char *p;
    LONG n;

    lhh_prefix(buf, LHH_DBG_BUFSIZE, &n);
    p = buf + n;
    RawDoFmt((char *)fmt, (LONG *)((ULONG)&fmt + sizeof(STRPTR)),
        (APTR)lhh_putch, (APTR)&p);
    if (p < buf + LHH_DBG_BUFSIZE - 1) {
        *p = '\0';
    } else {
        buf[LHH_DBG_BUFSIZE - 1] = '\0';
    }
    lhh_log_emit(buf);
}

void lhh_db2(const char *fmt, ...)
{
    static char buf[LHH_DBG_BUFSIZE];
    char *p;
    LONG n;

    lhh_prefix(buf, LHH_DBG_BUFSIZE, &n);
    p = buf + n;
    RawDoFmt((char *)fmt, (LONG *)((ULONG)&fmt + sizeof(STRPTR)),
        (APTR)lhh_putch, (APTR)&p);
    if (p < buf + LHH_DBG_BUFSIZE - 1) {
        *p = '\0';
    } else {
        buf[LHH_DBG_BUFSIZE - 1] = '\0';
    }
    lhh_log_emit(buf);
}

void lhh_db3(const char *fmt, ...)
{
    static char buf[LHH_DBG_BUFSIZE];
    char *p;
    LONG n;

    lhh_prefix(buf, LHH_DBG_BUFSIZE, &n);
    p = buf + n;
    RawDoFmt((char *)fmt, (LONG *)((ULONG)&fmt + sizeof(STRPTR)),
        (APTR)lhh_putch, (APTR)&p);
    if (p < buf + LHH_DBG_BUFSIZE - 1) {
        *p = '\0';
    } else {
        buf[LHH_DBG_BUFSIZE - 1] = '\0';
    }
    lhh_log_emit(buf);
}

const char *lhh_pkt_name(LONG type)
{
    const struct LhhPktName *p;

    for (p = lhh_pkt_names; p->name != NULL; p++) {
        if (p->type == type) {
            return p->name;
        }
    }
    return "?";
}

const char *lhh_lock_type_name(ULONG type)
{
    switch (type) {
    case LHH_LOCK_ROOT:
        return "ROOT";
    case LHH_LOCK_REAL:
        return "REAL";
    case LHH_LOCK_ARCHIVE:
        return "ARCHIVE";
    case LHH_LOCK_ENTRY:
        return "ENTRY";
    case LHH_LOCK_VINFO:
        return "VINFO";
    default:
        return "?";
    }
}

#endif /* LHH_DEBUG */
