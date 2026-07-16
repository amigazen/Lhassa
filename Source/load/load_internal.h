/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_internal.h - Load: install / ROLLBACK a Bin (system components MVP).
 *
 * LHA = Load / Handle / Archive.
 * Install: variant pick (CPU/FPU/MMU/WOS/PUP), OS:/Requires:, $VER gate,
 * copy-aside bak under SYS:T/Load, temp place + CRC16.
 * Absolute File: destinations (contain ':') ignore TO=.
 */

#ifndef LOAD_INTERNAL_H
#define LOAD_INTERNAL_H

#include <exec/types.h>
#include <dos/dos.h>

#define LOAD_PATH_LEN 512
#define LOAD_MAX_FILES 128
#define LOAD_MAX_REQS 32
#define LOAD_KEY_LEN 64
#define LOAD_VAL_LEN 256
#define LOAD_TOKEN_LEN 16
#define LOAD_TDIR "SYS:T/Load"
#define LOAD_VER_LINE_MAX 128

struct LoadArgs {
    STRPTR bin;
    STRPTR to;
    STRPTR dest;
    ULONG quiet;
    ULONG force;
    ULONG rollback;
    ULONG help;
    char bin_path[LOAD_PATH_LEN];
    char to_path[LOAD_PATH_LEN];
    char dest_path[LOAD_PATH_LEN];
};

/* One Manifest File: line (variant of a logical install target). */
struct LoadFileVar {
    char dest[LOAD_VAL_LEN];
    char from[LOAD_VAL_LEN];
    char cpu[16];
    int fpu881;
    int fpu882;
    int mmu;
    int wos;
    int pup;
    int selected;
};

struct LoadRequire {
    char name[LOAD_VAL_LEN];
    char minver[32];
    char url[LOAD_VAL_LEN];
};

struct LoadManifest {
    char name[LOAD_VAL_LEN];
    char version[LOAD_VAL_LEN];
    char install_root[LOAD_VAL_LEN];
    char os_min[32];
    struct LoadFileVar files[LOAD_MAX_FILES];
    LONG nfiles;
    struct LoadRequire reqs[LOAD_MAX_REQS];
    LONG nreqs;
};

/* UpdateSys-style $VER: major.minor and optional (dd.mm.yy) date. */
struct LoadVerInfo {
    int have_ver;
    int have_date;
    ULONG major;
    ULONG minor;
    ULONG ymd;
};

void load_print_usage(void);
void load_print_error(STRPTR msg, LONG code);

LONG load_path_is_absolute(STRPTR path);
LONG load_path_is_volume_root(STRPTR path);
LONG load_resolve_dest(STRPTR out, LONG outlen, STRPTR dest,
    STRPTR to_override, STRPTR install_root);

int load_host_cpu_rank(void);
int load_cpu_rank(STRPTR cpu);
LONG load_select_variants(struct LoadManifest *man);
LONG load_check_deps(struct LoadManifest *man);

LONG load_parse_manifest(STRPTR text, LONG len, struct LoadManifest *man);

/* load_ver.c */
void load_ver_clear(struct LoadVerInfo *vi);
LONG load_ver_parse_line(STRPTR line, struct LoadVerInfo *vi);
/*
 * 1 = parsed $VER:, 0 = I/O or unparseable $VER:, -1 = no $VER: tag.
 */
LONG load_ver_scan_file(STRPTR path, struct LoadVerInfo *vi);
LONG load_ver_parse_dotted(STRPTR s, ULONG *major, ULONG *minor);
int load_ver_compare(const struct LoadVerInfo *cand,
    const struct LoadVerInfo *inst);

/* load_atomic.c — SYS:T/Load bak + CRC16 place */
UWORD load_crc16_buf(APTR data, LONG len);
LONG load_crc16_file(STRPTR path, UWORD *crc_out);
LONG load_ensure_tdir(void);
LONG load_ensure_parent(STRPTR path);
LONG load_copy_file(STRPTR src, STRPTR dst);
LONG load_write_file(STRPTR path, APTR data, LONG len);
LONG load_clear_write_protect(STRPTR path);
LONG load_alloc_token(STRPTR token, LONG tokenlen);
LONG load_bak_path(STRPTR out, LONG outlen, STRPTR token);
LONG load_sidecar_path(STRPTR out, LONG outlen, STRPTR token);
LONG load_write_sidecar(STRPTR token, STRPTR dest, STRPTR bak,
    const struct LoadVerInfo *ver, UWORD crc);
LONG load_backup_to_t(STRPTR dest, STRPTR token_out, LONG tokenlen,
    STRPTR bak_out, LONG baklen);
LONG load_place_file(STRPTR cand, STRPTR dest, UWORD expect_crc,
    STRPTR bak_path);
LONG load_rollback_dest(STRPTR dest, ULONG quiet);

LONG load_run(struct LoadArgs *args);

#endif /* LOAD_INTERNAL_H */
