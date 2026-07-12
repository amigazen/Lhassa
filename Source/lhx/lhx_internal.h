/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx_internal.h - LhX Amiga-native CLI (lh.library; dos/utility APIs).
 */

#ifndef LHX_INTERNAL_H
#define LHX_INTERNAL_H

#include <exec/types.h>
#include <dos/dos.h>
#include <workbench/startup.h>

#define LHX_PATH_LEN 512
#define LHX_PASS_LEN 256
#define LHX_MAX_PATTERNS 64

/* Temporary tracing to ErrorOutput(); set 0 to disable. */
#define LHX_DEBUG 0

struct LhxArgs {
    ULONG list;
    ULONG extract;
    ULONG extractflat;
    ULONG add;
    ULONG test;
    ULONG print;
    ULONG help;
    STRPTR archive;
    STRPTR *files;
    STRPTR destdir;
    ULONG quiet;
    ULONG force;
    ULONG nopaths;
    STRPTR password;
    /* Stable copies; ReadArgs buffers are freed before lh.library calls. */
    char archive_path[LHX_PATH_LEN];
    char destdir_path[LHX_PATH_LEN];
    char password_buf[LHX_PASS_LEN];
};

void lhx_print_usage(void);
void lhx_print_error(STRPTR msg, LONG code);

void lhx_dbg_s(STRPTR label, STRPTR value);
void lhx_dbg_l(STRPTR label, LONG value);
void lhx_dbg_ll(STRPTR label, LONG a, LONG b);
void lhx_dbg_dump_args(struct LhxArgs *args);

int lhx_name_matches(STRPTR pattern, STRPTR name);
int lhx_any_selected(STRPTR name, STRPTR *patterns);
int lhx_basename(STRPTR path, STRPTR out, LONG outlen);
int lhx_path_join(STRPTR out, LONG outlen, STRPTR dir, STRPTR name);
int lhx_ensure_parent_dir(STRPTR path);
LONG lhx_read_file(STRPTR path, APTR *data, LONG *len);

/* Output: list command style (LIST), lhex style (EXTRACT/TEST/ADD). */
void lhx_list_entry(struct LhxArgs *args, struct FileInfoBlock *fib);
void lhx_list_total(LONG files, LONG blocks);
void lhx_work_start(struct LhxArgs *args, STRPTR name);
void lhx_work_done(struct LhxArgs *args, LONG test_mode);
void lhx_add_line(struct LhxArgs *args, STRPTR name);

LONG lhx_cmd_list(struct LhxArgs *args);
LONG lhx_cmd_extract(struct LhxArgs *args, LONG with_paths);
LONG lhx_cmd_add(struct LhxArgs *args);
LONG lhx_cmd_test(struct LhxArgs *args);
LONG lhx_cmd_print(struct LhxArgs *args);

/* Workbench startup: remap project path to LHA: and open as drawer. */
int lhx_real_to_lha_path(STRPTR real, STRPTR out, LONG outlen);
LONG lhx_wb_startup(struct WBStartup *wb);

#endif /* LHX_INTERNAL_H */
