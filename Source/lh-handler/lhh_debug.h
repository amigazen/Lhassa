/*
 * lhh_debug.h - Optional file log via child process (normal dos Open/Write).
 *
 * Default build: logging compiled out (no child, log file).
 * Debug build:  smake with DEF=LHH_DEBUG (see smakefile).
 *
 * Default path when enabled: T:LH-handler.log
 * (fallback RAM:LH-handler.log)
 */

#ifndef LHH_DEBUG_H
#define LHH_DEBUG_H

#ifndef LHH_LOG_PATH
#define LHH_LOG_PATH "AmigaZen:lhasa/LH-handler.log"
#endif
#ifndef LHH_LOG_FALLBACK
#define LHH_LOG_FALLBACK "T:LH-handler.log"
#endif

#ifdef LHH_DEBUG

const char *lhh_pkt_name(LONG type);
const char *lhh_lock_type_name(ULONG type);

void lhh_log_open(const char *path);
void lhh_log_close(void);
int lhh_log_active(void);
const char *lhh_log_path_used(void);

void lhh_db_pkt_enter(void);
void lhh_db_pkt_leave(void);

void lhh_db1(const char *fmt, ...);
void lhh_db2(const char *fmt, ...);
void lhh_db3(const char *fmt, ...);

#define DB(s)           lhh_db1("%s", (s))
#define DB1(s, a)       lhh_db1((s), (a))
#define DB2(s, a, b)    lhh_db2((s), (a), (b))
#define DB3(s, a, b, c) lhh_db3((s), (a), (b), (c))

#else /* !LHH_DEBUG */

#define lhh_log_open(p)         ((void)0)
#define lhh_log_close()         ((void)0)
#define lhh_log_active()        (0)
#define lhh_log_path_used()     ((const char *)"")
#define lhh_db_pkt_enter()      ((void)0)
#define lhh_db_pkt_leave()      ((void)0)

#define DB(s)                   ((void)0)
#define DB1(s, a)               ((void)0)
#define DB2(s, a, b)            ((void)0)
#define DB3(s, a, b, c)         ((void)0)

#endif /* LHH_DEBUG */

#endif /* LHH_DEBUG_H */
