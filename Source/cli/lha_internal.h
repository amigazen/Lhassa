/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_internal.h - Shared declarations for the LHA-compatible CLI.
 */

#ifndef LHA_INTERNAL_H
#define LHA_INTERNAL_H

#include "../include/lh.h"

typedef enum lha_cmd {
    LHA_CMD_NONE = 0,
    LHA_CMD_ADD,
    LHA_CMD_EXTRACT,
    LHA_CMD_EXTRACT_FLAT,
    LHA_CMD_LIST,
    LHA_CMD_VIEW,
    LHA_CMD_UPDATE,
    LHA_CMD_DELETE,
    LHA_CMD_MOVE,
    LHA_CMD_CONCAT,
    LHA_CMD_PRINT,
    LHA_CMD_TEST,
    LHA_CMD_FRESHEN,
    LHA_CMD_HUNT,
    LHA_CMD_REPLACE,
    LHA_CMD_COPY
} lha_cmd;

typedef struct lha_opts {
    lh_level level;
    int store_only;
    int header_level;
    int debug_verbose;
    int quiet;
    int quiet_level;
    int cmd_quiet;
    int view_full;
    int no_execute;
    int force;
    int text_mode;
    int delete_source;
    int ignore_paths;
    int preserve_paths;
    int generic_format;
    char workdir[512];
    /* LhA compatibility no-ops (cli.txt). */
    int noop_euc;
    int noop_no_progress;
    int noop_ignore_filenotes;
    int noop_fast_progress;
    int noop_extract_new;
    int noop_extract_newer;
    int noop_arc_newest_date;
    int noop_alt_progress;
    int noop_filelist;
    int noop_ignore_lhaopts;
    int noop_compress_archives;
    int noop_no_homedirs;
    int noop_lower_names;
    int noop_upper_names;
    int noop_workdir_flag;
    int noop_exclude;
    int noop_recurse;
    int noop_recurse_archives;
    int noop_add_no_arc;
    int noop_set_arc;
} lha_opts;

typedef struct lha_args {
    lha_cmd cmd;
    char *archive;
    char **files;
    int file_count;
    char *destdir;
} lha_args;

extern lha_opts g_opts;

void lha_opts_init(lha_opts *o);
void lha_usage(const char *prog);
int lha_parse_args(int argc, char **argv, lha_args *out, char **unknown_opt);
void lha_args_free(lha_args *a);

char *lha_strdup(const char *s);
void lha_normalize_slashes(char *s);
char *lha_basename(const char *path);
int lha_path_join(char *out, size_t outsz, const char *dir, const char *name);
int lha_mkpath(const char *path);
int lha_match_pattern(const char *pattern, const char *name);
int lha_name_matches_any(const char *name, char **patterns, int count);
int lha_read_file(const char *path, unsigned char **data, size_t *len, lh_datetime *dt);
int lha_write_file(const char *path, const unsigned char *data, size_t len);

lh_status lha_rewrite_archive(
    const char *archive,
    int (*keep)(const lh_entry *entry, void *ctx),
    void *ctx
);

int lha_run_command(const lha_args *args);

const char *lha_archive_name_for_display(const char *path);
void lha_extract_banner(const char *archive);
void lha_test_banner(const char *archive);
void lha_extract_summary(unsigned long count, unsigned long bad);
void lha_test_summary(unsigned long count, unsigned long bad);
void lha_operation_success(void);
void lha_operation_failed(void);
void lha_add_message(const char *name);

void lha_extract_progress_begin(void *ctx, const char *name, size_t size);
void lha_test_progress_begin(void *ctx, const char *name, size_t size);
void lha_progress_update(void *ctx, size_t done, size_t total);
void lha_progress_end(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size);
void lha_test_progress_end(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size);

#endif /* LHA_INTERNAL_H */
