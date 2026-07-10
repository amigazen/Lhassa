/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_parse.c - LHA 2.x / 1.14i command-line parsing (Amiga LhA compatible).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lha_internal.h"

lha_opts g_opts;

void lha_opts_init(lha_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->level = LH_LEVEL_LH5;
    o->header_level = 2;
    o->preserve_paths = -1;
}

void lha_usage(const char *prog)
{
    fputs("\n", stderr);
    fputs("LhA Freeware Version 2.15 68020+\n", stderr);
    fputs("Copyright (c) 1991-94 by Stefan Boberg.\n", stderr);
    fputs("Copyright (c) 1998,1999 by Jim Cooper and David Tritscher.\n", stderr);
    fputs("Copyright (c) 2004-2011 by Sven Ottemann.\n", stderr);
    fputs("Port Lhassa (c) 2026 amigazen project.\n\n", stderr);
    fprintf(stderr,
        "Usage: %s [-<options>] <command> <archive[.LZH/LHA]> [[homedir]\n"
        "           <filespec>...] [@file] [destdir]\n\n", prog);
    fputs("                        Where <Command> is one of:\n\n", stderr);
    fputs("  a    Add files                       c    Concatenate/Append archives\n", stderr);
    fputs("  d    Delete files                    e    Extract files\n", stderr);
    fputs("  f    Freshen files                   h    Hunt for diffs arc <-> filesys\n", stderr);
    fputs("  l[q] List archive (terse)            m    Move files to archive\n", stderr);
    fputs("  p    Print files to stdout           r    Replace files\n", stderr);
    fputs("  t    Test archive integrity          u    Update archive\n", stderr);
    fputs("  v[q] List archive (verbose)          vv   Show archive contents (full)\n", stderr);
    fputs("  x    Extract files with full path    y    Copy archive with new options\n\n", stderr);
    fputs("                      And <Options> is one or more of:\n\n", stderr);
    fputs(" -a  Preserve file attributes (D)     -A  Set archive attributes (D)\n", stderr);
    fputs(" -b  Set I/O buffer size (KB)         -B  Keep backup of archive\n", stderr);
    fputs(" -c  Confirm files                    -C  Clear arc-bit on extract (D)\n", stderr);
    fputs(" -d  Archive date = newest file       -D  Alternate progress display\n", stderr);
    fputs(" -e  Archive empty directories        -E  Touch extracted files\n", stderr);
    fputs(" -f  Ignore filenotes                 -F  Use fast progress display\n", stderr);
    fputs(" -G  Only extract newer files         -h  Disable homedirectories\n", stderr);
    fputs(" -H  Write header level ( 0,1,2 )     -i  Read filelist from file\n", stderr);
    fputs(" -I  Ignore ENV:LHAOPTS               -k  Keep partial files\n", stderr);
    fputs(" -K  Kill empty directories (move)    -l  Make filenames lowercase\n", stderr);
    fputs(" -L  Create filelist                  -m  No messages for query\n", stderr);
    fputs(" -M  No autoshow files                -n  No byte progress indicator\n", stderr);
    fputs(" -N  No progress indicator            -o  On or after date (newer than)\n", stderr);
    fputs(" -O  On or before date (older than)   -p  Pause after loading\n", stderr);
    fputs(" -P  Set task priority                -q  Be quiet\n", stderr);
    fputs(" -Q  Alternate option set introducer  -r  Collect files recursively\n", stderr);
    fputs(" -R  Collect archives recursively     -s  Only add files with A flag unset\n", stderr);
    fputs(" -S  Set A flag on added files (D)    -t  Only extract new files\n", stderr);
    fputs(" -T  Only extract new & newer files   -u  Make filenames uppercase\n", stderr);
    fputs(" -V  Enable/set multivolume size(KB)  -w  Set work directory\n", stderr);
    fputs(" -W  Exclude filenames                -x  Preserve and use path names\n", stderr);
    fputs(" -X  Do not append LZH/LHA suffix     -y  Always append LZH/LHA suffix\n", stderr);
    fputs(" -Y  Store big files w.ratio < 3%     -z  Do not compress files\n", stderr);
    fputs(" -Z  Compress archives                -0  Use LhArc V1.x compression\n", stderr);
    fputs(" -2  Use LHA V2.x compression (-lh5-) -3  Use LHA V2.x compression (-lh6-)\n\n", stderr);
    fputs("                             Alternate options:\n\n", stderr);
    fputs(" -Qa Use simple console I/O           -Qb Test archive before extract\n", stderr);
    fputs(" -Qd Delete autoshow files            -Qh Set Huffman buffer size (KB)\n", stderr);
    fputs(" -Qm Use filename 'munging'           -Qn Set national character mode\n", stderr);
    fputs(" -Qo Ignore options after cmd         -Qp Ignore delete protection flag\n", stderr);
    fputs(" -Qq Quick add                        -Qr Skip datestamp check\n", stderr);
    fputs(" -Qv Set multivolume arc devices      -Qw Disable wildcards\n\n", stderr);
    fputs("  Options are case-sensitive and may be specified anywhere on the command\n", stderr);
    fputs("line, the option letter followed by a 0 disables the option, any non-zero\n", stderr);
    fputs("digit will enable it. If no digit follows the option letter, it will be\n", stderr);
    fputs("enabled. Some options are enabled by default (depending on used command),\n", stderr);
    fputs("the default compression is LHA V2.x-style (-lh5-) compression. For more\n", stderr);
    fputs("info please consult the user's manual.\n\n", stderr);
    fputs("  The destination directory must have a trailing slash (/) or colon (:).\n", stderr);
}

static int lha_is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int lha_consume_digits(const char *s, int *pos)
{
    int n;

    n = 0;
    while (s[*pos] && lha_is_digit(s[*pos])) {
        n = n * 10 + (s[*pos] - '0');
        (*pos)++;
    }
    return n;
}

static lha_cmd lha_parse_command(const char *s)
{
    if (!s || !s[0]) {
        return LHA_CMD_NONE;
    }
    if (strcmp(s, "vv") == 0) {
        g_opts.view_full = 1;
        return LHA_CMD_VIEW;
    }
    if (strcmp(s, "lq") == 0) {
        g_opts.cmd_quiet = 1;
        return LHA_CMD_LIST;
    }
    if (strcmp(s, "vq") == 0) {
        g_opts.cmd_quiet = 1;
        return LHA_CMD_VIEW;
    }
    if (s[1] == '\0') {
        switch (s[0]) {
        case 'a': return LHA_CMD_ADD;
        case 'c': return LHA_CMD_CONCAT;
        case 'd': return LHA_CMD_DELETE;
        case 'e': return LHA_CMD_EXTRACT_FLAT;
        case 'f': return LHA_CMD_FRESHEN;
        case 'h': return LHA_CMD_HUNT;
        case 'l': return LHA_CMD_LIST;
        case 'm': return LHA_CMD_MOVE;
        case 'p': return LHA_CMD_PRINT;
        case 'r': return LHA_CMD_REPLACE;
        case 't': return LHA_CMD_TEST;
        case 'u': return LHA_CMD_UPDATE;
        case 'v': return LHA_CMD_VIEW;
        case 'x': return LHA_CMD_EXTRACT;
        case 'y': return LHA_CMD_COPY;
        default: break;
        }
    }
    return LHA_CMD_NONE;
}

static void lha_apply_option_digit(char c, int val)
{
    switch (c) {
    case 'a':
    case 'A':
    case 'c':
    case 'C':
    case 'd':
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'G':
    case 'h':
    case 'i':
    case 'I':
    case 'k':
    case 'K':
    case 'l':
    case 'L':
    case 'm':
    case 'M':
    case 'n':
    case 'N':
    case 'o':
    case 'O':
    case 'p':
    case 'P':
    case 'q':
    case 'r':
    case 's':
    case 'S':
    case 't':
    case 'T':
    case 'u':
    case 'U':
    case 'V':
    case 'W':
    case 'w':
    case 'x':
    case 'X':
    case 'y':
    case 'Y':
    case 'z':
    case 'Z':
        (void)val;
        break;
    default:
        break;
    }
}

static int lha_apply_option_char(char c, int has_digit, int digit_val)
{
    if (has_digit) {
        lha_apply_option_digit(c, digit_val);
    }

    switch (c) {
    case 'q':
        g_opts.quiet = has_digit ? (digit_val != 0) : 1;
        if (has_digit && digit_val >= 0 && digit_val <= 2) {
            g_opts.quiet_level = digit_val;
        }
        break;
    case 'v':
        /* Secret debug flag; not shown in usage. */
        g_opts.debug_verbose = has_digit ? (digit_val != 0) : 1;
        break;
    case 'n':
        g_opts.noop_no_progress = has_digit ? (digit_val != 0) : 1;
        break;
    case 'N':
        g_opts.noop_no_progress = has_digit ? (digit_val != 0) : 1;
        break;
    case 'f':
        g_opts.noop_ignore_filenotes = has_digit ? (digit_val != 0) : 1;
        break;
    case 'F':
        g_opts.noop_fast_progress = has_digit ? (digit_val != 0) : 1;
        break;
    case 't':
        if (has_digit && digit_val == 0) {
            g_opts.noop_extract_new = 0;
        } else {
            g_opts.noop_extract_new = 1;
        }
        break;
    case 'T':
        g_opts.noop_extract_newer = has_digit ? (digit_val != 0) : 1;
        break;
    case 'd':
        g_opts.noop_arc_newest_date = has_digit ? (digit_val != 0) : 1;
        break;
    case 'D':
        g_opts.noop_alt_progress = has_digit ? (digit_val != 0) : 1;
        break;
    case 'o':
        if (!has_digit) {
            g_opts.level = LH_LEVEL_LH1;
        }
        break;
    case 'z':
        g_opts.store_only = has_digit ? (digit_val != 0) : 1;
        if (!has_digit || digit_val != 0) {
            g_opts.level = LH_LEVEL_STORE;
        }
        break;
    case 'Z':
        g_opts.noop_compress_archives = has_digit ? (digit_val != 0) : 1;
        break;
    case 'i':
        g_opts.noop_filelist = has_digit ? (digit_val != 0) : 1;
        break;
    case 'I':
        g_opts.noop_ignore_lhaopts = has_digit ? (digit_val != 0) : 1;
        break;
    case 'g':
        g_opts.generic_format = has_digit ? (digit_val != 0) : 1;
        if (!has_digit || digit_val != 0) {
            g_opts.header_level = 0;
        }
        break;
    case '0':
        g_opts.header_level = 0;
        g_opts.level = LH_LEVEL_STORE;
        break;
    case '1':
        g_opts.header_level = 1;
        break;
    case '2':
        g_opts.level = LH_LEVEL_LH5;
        break;
    case '3':
        g_opts.level = LH_LEVEL_LH6;
        break;
    case 'x':
        g_opts.preserve_paths = has_digit ? (digit_val != 0) : 1;
        g_opts.ignore_paths = 0;
        break;
    case 'h':
        g_opts.noop_no_homedirs = has_digit ? (digit_val != 0) : 1;
        break;
    case 'l':
        g_opts.noop_lower_names = has_digit ? (digit_val != 0) : 1;
        break;
    case 'u':
        g_opts.noop_upper_names = has_digit ? (digit_val != 0) : 1;
        break;
    case 'w':
        g_opts.noop_workdir_flag = has_digit ? (digit_val != 0) : 1;
        break;
    case 'W':
        g_opts.noop_exclude = has_digit ? (digit_val != 0) : 1;
        break;
    case 'r':
        g_opts.noop_recurse = has_digit ? (digit_val != 0) : 1;
        break;
    case 'R':
        g_opts.noop_recurse_archives = has_digit ? (digit_val != 0) : 1;
        break;
    case 's':
        g_opts.noop_add_no_arc = has_digit ? (digit_val != 0) : 1;
        break;
    case 'S':
        g_opts.noop_set_arc = has_digit ? (digit_val != 0) : 1;
        break;
    case 'a':
    case 'A':
    case 'b':
    case 'B':
    case 'c':
    case 'C':
    case 'e':
    case 'E':
    case 'k':
    case 'K':
    case 'L':
    case 'm':
    case 'M':
    case 'p':
    case 'P':
    case 'V':
    case 'X':
    case 'y':
    case 'Y':
    case 'Q':
        /* Accepted no-op for Amiga script compatibility. */
        break;
    default:
        return 0;
    }
    return 1;
}

static int lha_apply_option_cluster(const char *opt)
{
    int i;
    int digit_val;
    int has_digit;

    if (!opt || !opt[0]) {
        return 0;
    }
    if (strcmp(opt, "o5") == 0) { g_opts.level = LH_LEVEL_LH5; return 1; }
    if (strcmp(opt, "o6") == 0) { g_opts.level = LH_LEVEL_LH6; return 1; }
    if (strcmp(opt, "o7") == 0) { g_opts.level = LH_LEVEL_LH7; return 1; }
    if (strcmp(opt, "q0") == 0) { g_opts.quiet = 1; g_opts.quiet_level = 0; return 1; }
    if (strcmp(opt, "q1") == 0) { g_opts.quiet = 1; g_opts.quiet_level = 1; return 1; }
    if (strcmp(opt, "q2") == 0) { g_opts.quiet = 1; g_opts.quiet_level = 2; return 1; }
    if (strncmp(opt, "w=", 2) == 0) {
        strncpy(g_opts.workdir, opt + 2, sizeof(g_opts.workdir) - 1);
        g_opts.workdir[sizeof(g_opts.workdir) - 1] = '\0';
        return 1;
    }
    if (opt[0] == 'Q') {
        /* Alternate option introducer (-Qa, -Qm, ...): accept as no-op. */
        return 1;
    }

    i = 0;
    while (opt[i]) {
        char c;

        c = opt[i];
        i++;
        has_digit = 0;
        digit_val = 0;
        if (lha_is_digit(opt[i])) {
            has_digit = 1;
            digit_val = lha_consume_digits(opt, &i);
        }
        if (!lha_apply_option_char(c, has_digit, digit_val)) {
            return 0;
        }
    }
    return 1;
}

static int lha_looks_like_destdir(const char *path)
{
    size_t n;

    if (!path || !path[0]) {
        return 0;
    }
    n = strlen(path);
    if (path[n - 1] == '/' || path[n - 1] == '\\' || path[n - 1] == ':') {
        return 1;
    }
#if defined(LHA_AMIGA)
    /*
     * Amiga destination paths are often written without a trailing colon:
     *   lha x archive.lha ram:test
     * not only ram:test/ or ram:
     */
    if (strchr(path, ':') != NULL) {
        return 1;
    }
#endif
    return 0;
}

int lha_parse_args(int argc, char **argv, lha_args *out, char **unknown_opt)
{
    int i;
    int stop_opts;
    int cmd_idx;

    if (unknown_opt) {
        *unknown_opt = NULL;
    }
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    lha_opts_init(&g_opts);
    stop_opts = 0;
    cmd_idx = -1;

    for (i = 1; i < argc; i++) {
        if (!stop_opts && argv[i][0] == '-' && argv[i][1]) {
            if (strcmp(argv[i], "--") == 0) {
                stop_opts = 1;
                continue;
            }
            if (!lha_apply_option_cluster(argv[i] + 1)) {
                if (unknown_opt) {
                    *unknown_opt = lha_strdup(argv[i]);
                }
                return -1;
            }
            continue;
        }
        if (cmd_idx < 0) {
            out->cmd = lha_parse_command(argv[i]);
            if (out->cmd == LHA_CMD_NONE) {
                if (unknown_opt) {
                    *unknown_opt = lha_strdup(argv[i]);
                }
                return -1;
            }
            cmd_idx = i;
            continue;
        }
        if (!out->archive) {
            out->archive = lha_strdup(argv[i]);
            continue;
        }
        out->files = (char **)realloc(out->files, (size_t)(out->file_count + 1) * sizeof(char *));
        if (!out->files) {
            return -1;
        }
        out->files[out->file_count++] = lha_strdup(argv[i]);
    }

    if (g_opts.preserve_paths < 0) {
        if (out->cmd == LHA_CMD_EXTRACT) {
            g_opts.ignore_paths = 0;
        } else if (out->cmd == LHA_CMD_EXTRACT_FLAT) {
            g_opts.ignore_paths = 1;
        }
    }
    if (out->file_count > 0) {
        char *last = out->files[out->file_count - 1];
        if (out->cmd == LHA_CMD_EXTRACT || out->cmd == LHA_CMD_EXTRACT_FLAT) {
            if (lha_looks_like_destdir(last)) {
                out->destdir = last;
                out->file_count--;
            }
        }
    }
    return 1;
}

void lha_args_free(lha_args *a)
{
    int i;

    if (!a) {
        return;
    }
    free(a->archive);
    if (a->files) {
        for (i = 0; i < a->file_count; i++) {
            free(a->files[i]);
        }
        free(a->files);
    }
    memset(a, 0, sizeof(*a));
}
