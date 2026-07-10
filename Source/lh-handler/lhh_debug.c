/*
 * lhh_debug.c - File log via a child process.
 *
 * The handler process must not call dos.library Open/Write (those wait on
 * pr_MsgPort, which is also our packet port).  A small child process owns
 * the log file and uses normal Open/Write on its own pr_MsgPort.
 *
 * Parent sends log lines as messages; child writes them.  Lines are queued
 * in a ring during packet handling and flushed on pkt_leave.
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

struct LhhPktName {
    LONG type;
    const char *name;
};

static const struct LhhPktName lhh_pkt_names[] = {
    { 1,   "LOCATE_OBJECT" },
    { 2,   "FREE_LOCK" },
    { 3,   "FINDINPUT" },
    { 4,   "FINDOUTPUT" },
    { 5,   "FINDUPDATE" },
    { 6,   "END" },
    { 7,   "READ" },
    { 8,   "WRITE" },
    { 9,   "SEEK" },
    { 16,  "DIE" },
    { 17,  "COPY_DIR" },
    { 18,  "PARENT" },
    { 19,  "EXAMINE_OBJECT" },
    { 20,  "EXAMINE_NEXT" },
    { 21,  "INFO" },
    { 22,  "DISK_INFO" },
    { 23,  "RENAME" },
    { 24,  "DELETE_OBJECT" },
    { 25,  "SETCOMMENT" },
    { 26,  "PROTECT" },
    { 27,  "CREATE_DIR" },
    { 28,  "UPDATE" },
    { 29,  "DATE" },
    { 30,  "SAME_LOCK" },
    { 31,  "CHANGE_MODE" },
    { 32,  "CURRENT_VOLUME" },
    { 33,  "IS_FILESYSTEM" },
    { 34,  "EXAMINE_FH" },
    { 1025, "COPY_DIR_FH" },
    { 1033, "EXAMINE_ALL" },
    { 1035, "EXAMINE_ALL_END" },
    { 0,   NULL }
};

static char lhh_log_ring[LHH_LOG_RING_SIZE];
static LONG lhh_log_ring_used;
static LONG lhh_pkt_depth;
static int lhh_log_lazy_done;
static char lhh_log_pathbuf[128];

/* Set by child when its MsgPort is ready; cleared on quit. */
static volatile struct MsgPort *lhh_log_child_port;
static struct Process *lhh_log_child;
static struct Process *lhh_log_parent;
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
 * Log child: normal dos Open/Write on this process's own pr_MsgPort.
 * Uses AbsExecBase - never LhhGD (handler small-data / A4).
 */
#ifdef __SASC
static void __saveds lhh_log_child_entry(void)
#else
static void lhh_log_child_entry(void)
#endif
{
    struct Library *AbsSysBase;
    struct DosLibrary *AbsDOSBase;
    struct Process *pr;
    struct MsgPort *port;
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
    pr = (struct Process *)FindTask(NULL);
    port = &pr->pr_MsgPort;
    fh = ZERO;
    running = 1;
    used = NULL;

    AbsDOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (AbsDOSBase == NULL) {
        lhh_log_child_port = NULL;
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
    if (used != NULL) {
        for (j = 0; used[j] != '\0' && j < (LONG)sizeof(lhh_log_pathbuf) - 1;
            j++) {
            lhh_log_pathbuf[j] = used[j];
        }
        lhh_log_pathbuf[j] = '\0';
    }

    lhh_log_child_port = port;

    while (running) {
        WaitPort(port);
        while ((lm = (struct LhhLogMsg *)GetMsg(port)) != NULL) {
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
    lhh_log_child_port = NULL;
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
    struct TagItem tags[5];
    LONG i;

    if (lhh_log_child_port != NULL) {
        return 1;
    }
    if (LhhGD == NULL || DOSBase == NULL) {
        return 0;
    }

    lhh_log_parent = (struct Process *)FindTask(NULL);
    lhh_log_child_port = NULL;
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
    tags[i].ti_Tag = NP_Priority;
    tags[i].ti_Data = (ULONG)0;
    i++;
    tags[i].ti_Tag = TAG_DONE;
    tags[i].ti_Data = 0;

    lhh_log_child = CreateNewProc(tags);
    if (lhh_log_child == NULL) {
        return 0;
    }

    /* Busy-wait - Delay() on the handler mixes with pr_MsgPort packets. */
    for (i = 0; i < 1000000 && lhh_log_child_port == NULL; i++) {
        /* spin */
    }
    if (lhh_log_child_port == NULL) {
        return 0;
    }

    lhh_log_ok = 1;
    return 1;
}

static void lhh_log_send(const char *text, LONG len)
{
    struct LhhLogMsg *lm;
    struct MsgPort *dest;
    LONG i;
    LONG n;

    dest = (struct MsgPort *)lhh_log_child_port;
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
    dest = (struct MsgPort *)lhh_log_child_port;
    if (dest != NULL) {
        lm = (struct LhhLogMsg *)AllocMem(sizeof(struct LhhLogMsg),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (lm != NULL) {
            lm->msg.mn_Node.ln_Type = NT_MESSAGE;
            lm->msg.mn_Length = (UWORD)sizeof(struct LhhLogMsg);
            lm->len = LHH_LOG_CMD_QUIT;
            PutMsg(dest, &lm->msg);
        }
        for (i = 0; i < 200000 && lhh_log_child_port != NULL; i++) {
            /* spin until child clears port */
        }
    }
    lhh_log_ok = 0;
    lhh_log_child = NULL;
    lhh_log_ring_used = 0;
}

int lhh_log_active(void)
{
    return (lhh_log_ok && lhh_log_child_port != NULL) ? 1 : 0;
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
    default:
        return "?";
    }
}
