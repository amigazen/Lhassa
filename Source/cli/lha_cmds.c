/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_cmds.c - LHA 1.14i command implementations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lha_internal.h"

static const char *lha_month_name(int m)
{
    static const char *names[] = {
        "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (m < 1 || m > 12) {
        return "???";
    }
    return names[m];
}

/* 68000-safe unsigned division (vbcc may emit __ldivu). */
static unsigned long lha_udiv_ul(unsigned long n, unsigned long d)
{
    unsigned long q;
    unsigned long r;
    int i;

    if (d == 0) {
        return 0;
    }
    q = 0;
    r = 0;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1UL);
        if (r >= d) {
            r -= d;
            q |= (1UL << i);
        }
    }
    return q;
}

static void lha_split10(unsigned long v, unsigned long *tens, unsigned long *ones)
{
    unsigned long t;

    t = 0;
    while (v >= 10UL) {
        v -= 10UL;
        t++;
    }
    *tens = t;
    *ones = v;
}

static unsigned long lha_ratio_pct10(unsigned long packed, unsigned long orig)
{
    unsigned long saved;
    unsigned long q;
    unsigned long r;
    unsigned long pct;

    if (orig == 0) {
        return 0;
    }
    if (packed >= orig) {
        return 0;
    }
    saved = orig - packed;
    q = lha_udiv_ul(saved, orig);
    r = saved - q * orig;
    pct = q * 1000UL + lha_udiv_ul(r * 1000UL + (orig >> 1), orig);
    if (pct > 9999UL) {
        pct = 9999UL;
    }
    return pct;
}

static void lha_print_ratio(unsigned long packed, unsigned long orig)
{
    unsigned long pct10;
    unsigned long tens;
    unsigned long ones;

    if (orig == 0) {
        fputs("  0.0%", stdout);
        return;
    }
    pct10 = lha_ratio_pct10(packed, orig);
    lha_split10(pct10, &tens, &ones);
    fprintf(stdout, "%3lu.%lu%%", tens, ones);
}

static void lha_print_list_date(const lh_datetime *dt)
{
    int yr;

    yr = (int)dt->year % 100;
    fprintf(stdout, "%02d-%s-%02d",
        (int)dt->day, lha_month_name((int)dt->month), yr);
}

static void lha_print_list_time(const lh_datetime *dt)
{
    fprintf(stdout, "%02d:%02d:%02d",
        (int)dt->hour, (int)dt->minute, (int)dt->second);
}

static void lha_print_banner(void)
{
    if (g_opts.quiet || g_opts.noop_no_progress) {
        return;
    }
    fputs("LhA LhASsA Version 2.15 68000\n", stdout);
    fputs("Copyright (c) 2026 amigazen project\n\n", stdout);
}

static void lha_print_list_header(const char *archive)
{
    fprintf(stdout, "Listing of archive '%s':\n", archive);
    fputs("Original  Packed Ratio    Date     Time    Name\n", stdout);
    fputs("-------- ------- ----- --------- --------  -------------\n", stdout);
}

static void lha_print_list_row(
    unsigned long orig,
    unsigned long packed,
    const lh_datetime *dt,
    const char *name)
{
    fprintf(stdout, "%8lu %7lu ", orig, packed);
    lha_print_ratio(packed, orig);
    fputc(' ', stdout);
    lha_print_list_date(dt);
    fputc(' ', stdout);
    lha_print_list_time(dt);
    fprintf(stdout, "  %s\n", name);
}

static void lha_print_list_totals(
    unsigned long total_orig,
    unsigned long total_packed,
    unsigned long count,
    const lh_datetime *arc_dt)
{
    fprintf(stdout, "%8lu %7lu ", total_orig, total_packed);
    lha_print_ratio(total_packed, total_orig);
    fputc(' ', stdout);
    lha_print_list_date(arc_dt);
    fputc(' ', stdout);
    lha_print_list_time(arc_dt);
    fprintf(stdout, "   %lu file%s\n", count, count == 1 ? "" : "s");
}

static int lha_list_show_table(void)
{
    if (g_opts.cmd_quiet) {
        return 0;
    }
    return 1;
}

static int lha_list_show_messages(void)
{
    if (g_opts.quiet || g_opts.cmd_quiet) {
        return 0;
    }
    return 1;
}

static int lha_entry_selected(const lh_entry *entry, const lha_args *args)
{
    return lha_name_matches_any(entry->filename, args->files, args->file_count);
}

static const char *lha_dest_dir(const lha_args *args)
{
    if (args->destdir && args->destdir[0]) {
        return args->destdir;
    }
    if (g_opts.workdir[0]) {
        return g_opts.workdir;
    }
    return NULL;
}

static int lha_cmd_list(const lha_args *args, int verbose, int full)
{
    lh_reader *r;
    lh_entry entry;
    lh_status st;
    lh_status err;
    unsigned long total_orig;
    unsigned long total_packed;
    unsigned long count;
    lh_datetime arc_dt;
    int show_table;

    r = lh_reader_open(args->archive, &err);
    if (!r) {
        fprintf(stderr, "lha: cannot open %s: %s\n", args->archive, lh_status_string(err));
        return 1;
    }
    show_table = lha_list_show_table();
    if (show_table && lha_list_show_messages()) {
        lha_print_banner();
        lha_print_list_header(args->archive);
    }
    total_orig = 0;
    total_packed = 0;
    count = 0;
    lh_reader_set_header_only(r, 1);
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(r, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            fprintf(stderr, "lha: %s\n", lh_status_string(st));
            lh_reader_close(&r);
            return 1;
        }
        if (!lha_entry_selected(&entry, args)) {
            lh_entry_clear(&entry);
            continue;
        }
        if (show_table) {
            if (full) {
                fprintf(stdout, "%-48s %8lu %8lu %s crc=%s attr=0x%02x os=%c\n",
                    entry.filename,
                    (unsigned long)entry.packed_len,
                    (unsigned long)entry.data_len,
                    lh_method_to_string(entry.method),
                    entry.crc_ok ? "ok" : "bad",
                    (unsigned int)entry.attrs,
                    entry.os_id ? (char)entry.os_id : '?');
            } else if (verbose) {
                fprintf(stdout, "%8lu %7lu ",
                    (unsigned long)entry.data_len,
                    (unsigned long)entry.packed_len);
                lha_print_ratio((unsigned long)entry.packed_len,
                    (unsigned long)entry.data_len);
                fputc(' ', stdout);
                lha_print_list_date(&entry.datetime);
                fputc(' ', stdout);
                lha_print_list_time(&entry.datetime);
                fprintf(stdout, "  %s  %s\n",
                    entry.filename,
                    lh_method_to_string(entry.method));
            } else {
                lha_print_list_row(
                    (unsigned long)entry.data_len,
                    (unsigned long)entry.packed_len,
                    &entry.datetime,
                    entry.filename);
            }
            total_orig += (unsigned long)entry.data_len;
            total_packed += (unsigned long)entry.packed_len;
            count++;
        } else if (!g_opts.quiet) {
            fprintf(stdout, "%s\n", entry.filename);
        }
        lh_entry_clear(&entry);
    }
    if (show_table) {
        fputs("-------- ------- ----- --------- --------\n", stdout);
        if (!lh_reader_archive_datetime(r, &arc_dt)) {
            memset(&arc_dt, 0, sizeof(arc_dt));
        }
        lha_print_list_totals(total_orig, total_packed, count, &arc_dt);
        if (lha_list_show_messages()) {
            fputs("\nOperation successful.\n", stdout);
        }
    }
    lh_reader_close(&r);
    return 0;
}

static int lha_cmd_extract(const lha_args *args, int with_paths)
{
    lh_reader *r;
    lh_entry entry;
    lh_status st;
    lh_status err;
    char outpath[512];
    const char *dest;
    char *name;
    unsigned long count;
    unsigned long bad;

    dest = lha_dest_dir(args);
    r = lh_reader_open(args->archive, &err);
    if (!r) {
        fprintf(stderr, "lha: cannot open %s\n", args->archive);
        return 1;
    }
    lha_progress_set_filter(args);
    lh_reader_set_progress(r, lha_extract_progress_begin, lha_progress_update,
        lha_progress_end, NULL);
    lha_extract_banner(args->archive);
    count = 0;
    bad = 0;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(r, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            fprintf(stderr, "lha: %s\n", lh_status_string(st));
            lha_progress_set_filter(NULL);
            lh_reader_close(&r);
            return 1;
        }
        if (!lha_entry_selected(&entry, args)) {
            lh_entry_clear(&entry);
            continue;
        }
        if (entry.is_directory) {
            lh_entry_clear(&entry);
            continue;
        }
        if (with_paths && !g_opts.ignore_paths) {
            name = entry.filename;
        } else {
            name = lha_basename(entry.filename);
        }
        if (!lha_path_join(outpath, sizeof(outpath), dest, name)) {
            fprintf(stderr, "lha: path too long for %s\n", entry.filename);
            free(name != entry.filename ? name : NULL);
            lh_entry_clear(&entry);
            continue;
        }
        if (name != entry.filename) {
            free(name);
        }
        if (g_opts.no_execute) {
            printf("EXTRACT %s\n", outpath);
            count++;
        } else if (!lha_write_file(outpath, entry.data, entry.data_len)) {
            fprintf(stderr, "lha: cannot write %s\n", outpath);
        } else {
            lha_apply_file_metadata(outpath, &entry.datetime, entry.attrs);
            count++;
        }
        if (!entry.crc_ok) {
            bad++;
        }
        lh_entry_clear(&entry);
    }
    lh_reader_close(&r);
    lha_progress_set_filter(NULL);
    lha_extract_summary(count, bad);
    if (bad > 0) {
        lha_operation_failed();
    } else {
        lha_operation_success();
    }
    return bad > 0 ? 1 : 0;
}

static int lha_cmd_test(const lha_args *args)
{
    lh_reader *r;
    lh_entry entry;
    lh_status st;
    lh_status err;
    unsigned long count;
    unsigned long bad;

    r = lh_reader_open(args->archive, &err);
    if (!r) {
        fprintf(stderr, "lha: cannot open %s\n", args->archive);
        return 1;
    }
    lh_reader_set_progress(r, lha_test_progress_begin, lha_progress_update,
        lha_test_progress_end, NULL);
    lha_test_banner(args->archive);
    count = 0;
    bad = 0;
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(r, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            fprintf(stderr, "lha: %s\n", lh_status_string(st));
            lh_reader_close(&r);
            return 1;
        }
        if (!lha_entry_selected(&entry, args)) {
            lh_entry_clear(&entry);
            continue;
        }
        if (entry.is_directory) {
            lh_entry_clear(&entry);
            continue;
        }
        if (!entry.crc_ok) {
            bad++;
        }
        count++;
        lh_entry_clear(&entry);
    }
    lh_reader_close(&r);
    lha_test_summary(count, bad);
    if (bad > 0) {
        lha_operation_failed();
    } else {
        lha_operation_success();
    }
    return bad > 0 ? 1 : 0;
}

static int lha_cmd_print(const lha_args *args)
{
    lh_reader *r;
    lh_entry entry;
    lh_status st;
    lh_status err;

    r = lh_reader_open(args->archive, &err);
    if (!r) {
        return 1;
    }
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(r, &entry);
        if (st != LH_OK) {
            lh_reader_close(&r);
            return 1;
        }
        if (!entry.filename) {
            break;
        }
        if (!lha_entry_selected(&entry, args)) {
            lh_entry_clear(&entry);
            continue;
        }
        if (entry.data && entry.data_len > 0) {
            fwrite(entry.data, 1, entry.data_len, stdout);
        }
        lh_entry_clear(&entry);
    }
    lh_reader_close(&r);
    return 0;
}

static int lha_cmd_add(const lha_args *args)
{
    lh_writer *w;
    lh_status st;
    lh_status err;
    int i;
    unsigned char *data;
    size_t len;
    lh_datetime dt;
    char *arcname;

    if (args->file_count <= 0) {
        fprintf(stderr, "lha: no files specified\n");
        return 1;
    }
    w = lh_writer_open_append(args->archive, &err);
    if (!w) {
        fprintf(stderr, "lha: cannot open %s\n", args->archive);
        return 1;
    }
    for (i = 0; i < args->file_count; i++) {
        if (!lha_read_file(args->files[i], &data, &len, &dt)) {
            fprintf(stderr, "lha: cannot read %s\n", args->files[i]);
            continue;
        }
        arcname = lha_basename(args->files[i]);
        lha_add_message(arcname);
        st = lh_writer_add(w, arcname, NULL, LH_ATTR_DEFAULT, &dt,
            g_opts.level, g_opts.store_only, data, len);
        free(arcname);
        free(data);
        if (st != LH_OK) {
            fprintf(stderr, "lha: add failed: %s\n", lh_status_string(st));
            lh_writer_close(&w);
            return 1;
        }
    }
    lh_writer_close(&w);
    lha_operation_success();
    return 0;
}

typedef struct lha_keep_ctx {
    const lha_args *args;
    int invert;
} lha_keep_ctx;

static int lha_keep_not_selected(const lh_entry *entry, void *ctx)
{
    lha_keep_ctx *k;

    k = (lha_keep_ctx *)ctx;
    if (!entry || !entry->filename) {
        return 0;
    }
    if (k->invert) {
        return !lha_entry_selected(entry, k->args);
    }
    return lha_entry_selected(entry, k->args);
}

static int lha_keep_all(const lh_entry *entry, void *ctx)
{
    (void)ctx;
    return entry && entry->filename;
}

static int lha_cmd_delete(const lha_args *args)
{
    lha_keep_ctx k;
    lh_status st;

    k.args = args;
    k.invert = 1;
    if (g_opts.no_execute) {
        return 0;
    }
    st = lha_rewrite_archive(args->archive, lha_keep_not_selected, &k);
    if (st != LH_OK) {
        fprintf(stderr, "lha: delete failed: %s\n", lh_status_string(st));
        return 1;
    }
    return 0;
}

static int lha_cmd_concat(const lha_args *args)
{
    lh_reader *r;
    lh_writer *w;
    lh_entry entry;
    lh_status st;
    lh_status err;
    int i;

    w = lh_writer_open_append(args->archive, &err);
    if (!w) {
        return 1;
    }
    for (i = 0; i < args->file_count; i++) {
        r = lh_reader_open(args->files[i], &err);
        if (!r) {
            continue;
        }
        for (;;) {
            memset(&entry, 0, sizeof(entry));
            st = lh_reader_next(r, &entry);
            if (st != LH_OK || !entry.filename) {
                break;
            }
            st = lh_writer_add(w, entry.filename, entry.comment, entry.attrs,
                &entry.datetime, g_opts.level, g_opts.store_only,
                entry.data, entry.data_len);
            lh_entry_clear(&entry);
            if (st != LH_OK) {
                break;
            }
        }
        lh_reader_close(&r);
    }
    lh_writer_close(&w);
    return 0;
}

int lha_run_command(const lha_args *args)
{
    if (!args || !args->archive) {
        return 1;
    }
    lh_set_debug_verbose(g_opts.debug_verbose);
    switch (args->cmd) {
    case LHA_CMD_LIST:
        return lha_cmd_list(args, 0, 0);
    case LHA_CMD_VIEW:
        return lha_cmd_list(args, 1, g_opts.view_full);
    case LHA_CMD_EXTRACT:
        return lha_cmd_extract(args, 1);
    case LHA_CMD_EXTRACT_FLAT:
        return lha_cmd_extract(args, 0);
    case LHA_CMD_TEST:
        return lha_cmd_test(args);
    case LHA_CMD_PRINT:
        return lha_cmd_print(args);
    case LHA_CMD_ADD:
        return lha_cmd_add(args);
    case LHA_CMD_DELETE:
        return lha_cmd_delete(args);
    case LHA_CMD_CONCAT:
        return lha_cmd_concat(args);
    case LHA_CMD_UPDATE:
    case LHA_CMD_MOVE:
    case LHA_CMD_FRESHEN:
    case LHA_CMD_REPLACE:
    case LHA_CMD_HUNT:
    case LHA_CMD_COPY:
        fprintf(stderr, "lha: command not yet implemented\n");
        return 1;
    default:
        return 1;
    }
}
