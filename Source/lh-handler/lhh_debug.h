/*
 * lhh_debug.h - File log via child process (normal dos Open/Write).
 *
 * Default: AmigaZen:lhasa/LH-handler.log  (fallback RAM:LH-handler.log)
 */

#ifndef LHH_DEBUG_H
#define LHH_DEBUG_H

#ifndef LHH_LOG_PATH
#define LHH_LOG_PATH "AmigaZen:lhasa/LH-handler.log"
#endif
#ifndef LHH_LOG_FALLBACK
#define LHH_LOG_FALLBACK "RAM:LH-handler.log"
#endif

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

#endif /* LHH_DEBUG_H */
