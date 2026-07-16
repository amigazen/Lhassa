/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_internal.h - Handle: pack a stage into a Bin-profile LHA.
 *
 * System package mode: FROM directory is named SYS (Libs/, Devs/, ...).
 * Application package mode: any other FROM directory.
 *
 * Tags: .020 .030 .040 .060 .881 .882 .mmu .wos .pup (optional, combinable).
 * Optional staged Manifest supplies Summary/OS/Requires; File: lines generated.
 */

#ifndef HANDLE_INTERNAL_H
#define HANDLE_INTERNAL_H

#include <exec/types.h>
#include <dos/dos.h>

#define HANDLE_PATH_LEN 512
#define HANDLE_MAX_FILES 128
#define HANDLE_MAX_REQS 32
#define HANDLE_VAL_LEN 256
#define HANDLE_VER_LINE_MAX 128
#define HANDLE_MANIFEST_MAX 12288

/* Bin profile: LH0 stored member; canonical $VER line for the package. */
#define HANDLE_BIN_PACKAGE_VER "-package.ver"
/* LhA autoshow (filename ends in .displayme); warns to use Load. */
#define HANDLE_BIN_AUTOSHOW    "Load.displayme"

#define HANDLE_MODE_SYS 1
#define HANDLE_MODE_APP 0

struct HandleArgs {
    STRPTR from;
    STRPTR name;
    STRPTR primary;
    STRPTR version;
    STRPTR out;
    ULONG quiet;
    ULONG force;
    ULONG help;
    ULONG sys_mode;
    char from_path[HANDLE_PATH_LEN];
    char out_path[HANDLE_PATH_LEN];
    char name_buf[HANDLE_VAL_LEN];
    char primary_buf[HANDLE_PATH_LEN];
    char version_buf[32];
    char out_dir[HANDLE_PATH_LEN];
};

struct HandleVariant {
    int cpu_rank;   /* 0,20,30,40,60 */
    int fpu881;     /* .881 — 68881/68882/embedded FPU */
    int fpu882;     /* .882 — 68882 */
    int mmu;        /* .mmu */
    int wos;        /* .wos WarpOS */
    int pup;        /* .pup PowerUp */
};

struct HandleFileVar {
    char dest[HANDLE_VAL_LEN];
    char from[HANDLE_VAL_LEN];
    char cpu[16];
    int fpu881;
    int fpu882;
    int mmu;
    int wos;
    int pup;
};

struct HandleVerInfo {
    int have_ver;
    int have_date;
    ULONG major;
    ULONG minor;
    ULONG ymd;
};

struct HandleRequire {
    char name[HANDLE_VAL_LEN];
    char minver[32];
    char url[HANDLE_VAL_LEN];
};

struct HandleScan {
    ULONG sys_mode;
    char name[HANDLE_VAL_LEN];
    char version[HANDLE_VAL_LEN];
    struct HandleVerInfo pkg_ver;  /* PRIMARY $VER (for -package.ver member) */
    char os_min[32];
    char summary[HANDLE_VAL_LEN];
    struct HandleFileVar files[HANDLE_MAX_FILES];
    LONG nfiles;
    struct HandleRequire reqs[HANDLE_MAX_REQS];
    LONG nreqs;
};

void handle_print_usage(void);
void handle_print_error(STRPTR msg, LONG code);

LONG handle_is_hidden_name(STRPTR name);
ULONG handle_detect_sys_mode(STRPTR from_path);
LONG handle_build_out_path(struct HandleArgs *args, STRPTR out, LONG outlen);

LONG handle_read_file(STRPTR path, APTR *data, LONG *len);
LONG handle_path_join(STRPTR out, LONG outlen, STRPTR dir, STRPTR name);
LONG handle_arc_join(STRPTR out, LONG outlen, STRPTR rel, STRPTR name);

void handle_variant_clear(struct HandleVariant *v);
LONG handle_variant_parse_leaf(STRPTR leaf, STRPTR base_out, LONG baselen,
    struct HandleVariant *v);
void handle_variant_cpu_string(const struct HandleVariant *v, STRPTR out,
    LONG outlen);

void handle_ver_clear(struct HandleVerInfo *vi);
LONG handle_ver_scan_file(STRPTR path, struct HandleVerInfo *vi);
LONG handle_ver_parse_dotted(STRPTR s, ULONG *major, ULONG *minor);
LONG handle_build_package_ver(struct HandleScan *scan, STRPTR out, LONG outlen);

LONG handle_scan_stage(struct HandleArgs *args, struct HandleScan *scan);
LONG handle_build_manifest(struct HandleScan *scan, STRPTR out, LONG outlen);

/*
 * Locate NAME.readme beside FROM (parent), in cwd, or under FROM.
 * Fills disk path and archive member name (NAME.readme). Returns 1 if found.
 */
LONG handle_find_readme(struct HandleArgs *args, STRPTR disk_out, LONG outlen,
    STRPTR arc_out, LONG arclen);

LONG handle_run(struct HandleArgs *args);

#endif /* HANDLE_INTERNAL_H */
