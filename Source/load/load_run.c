/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_run.c - Manifest parse, variant pick, deps, $VER gate, install, ROLLBACK.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "load_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct Library *LhBase;

#ifndef AFF_68020
#define AFF_68020 (1L << 1)
#endif
#ifndef AFF_68030
#define AFF_68030 (1L << 2)
#endif
#ifndef AFF_68040
#define AFF_68040 (1L << 3)
#endif
#ifndef AFF_68881
#define AFF_68881 (1L << 4)
#endif
#ifndef AFF_68882
#define AFF_68882 (1L << 5)
#endif
#ifndef AFF_FPU40
#define AFF_FPU40 (1L << 6)
#endif
#ifndef AFF_68060
#define AFF_68060 (1L << 7)
#endif

void load_print_error(STRPTR msg, LONG code)
{
    if (!msg) {
        return;
    }
    if (code) {
        Printf("Load: %s (IoErr %ld)\n", (LONG)msg, code);
    } else {
        Printf("Load: %s\n", (LONG)msg);
    }
    Flush(Output());
}

void load_print_usage(void)
{
    Printf("Load BIN=archive.lha [TO=dir] [FORCE] [QUIET]\n", 0);
    Printf("Load ROLLBACK DEST=path [QUIET]\n", 0);
    Printf("  Install a Bin via Manifest File: map (CPU/FPU/MMU/WOS/PUP).\n", 0);
    Printf("  OS: checks exec.library; Requires: must be present.\n", 0);
    Printf("  $VER: gates upgrades; bak copies live under SYS:T/Load.\n", 0);
    Printf("  TO= relocates relative destinations only; LIBS:/C: stay fixed.\n", 0);
    Flush(Output());
}

LONG load_path_is_absolute(STRPTR path)
{
    LONG i;

    if (!path) {
        return 0;
    }
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == ':') {
            return 1;
        }
    }
    return 0;
}

LONG load_path_is_volume_root(STRPTR path)
{
    LONG len;
    STRPTR fp;

    if (!path || !path[0]) {
        return 1;
    }
    len = 0;
    while (path[len] != '\0') {
        len++;
    }
    if (path[len - 1] == ':') {
        return 1;
    }
    fp = FilePart(path);
    if (!fp || !fp[0]) {
        return 1;
    }
    return 0;
}

LONG load_resolve_dest(STRPTR out, LONG outlen, STRPTR dest,
    STRPTR to_override, STRPTR install_root)
{
    STRPTR base;
    char seg[LOAD_VAL_LEN];
    LONG i;
    LONG j;

    if (!out || outlen <= 0 || !dest || !dest[0]) {
        return 0;
    }

    if (load_path_is_absolute(dest)) {
        Strncpy(out, dest, outlen);
        if (load_path_is_volume_root(out)) {
            return 0;
        }
        return 1;
    }

    base = NULL;
    if (to_override && to_override[0]) {
        base = to_override;
    } else if (install_root && install_root[0]) {
        base = install_root;
    } else {
        base = (STRPTR)"Work:";
    }

    Strncpy(out, base, outlen);

    i = 0;
    while (dest[i] != '\0') {
        while (dest[i] == '/') {
            i++;
        }
        if (dest[i] == '\0') {
            break;
        }
        j = 0;
        while (dest[i] != '\0' && dest[i] != '/' && j < (LONG)sizeof(seg) - 1) {
            seg[j++] = dest[i++];
        }
        seg[j] = '\0';
        if (seg[0] == '\0') {
            continue;
        }
        if (!AddPart(out, seg, (ULONG)outlen)) {
            return 0;
        }
    }
    if (load_path_is_volume_root(out)) {
        return 0;
    }
    return 1;
}

int load_host_cpu_rank(void)
{
    UWORD attn;

    attn = SysBase->AttnFlags;
    if (attn & AFF_68060) {
        return 60;
    }
    if (attn & AFF_68040) {
        return 40;
    }
    if (attn & AFF_68030) {
        return 30;
    }
    if (attn & AFF_68020) {
        return 20;
    }
    return 0;
}

static int load_host_has_fpu881(void)
{
    UWORD attn;

    attn = SysBase->AttnFlags;
    if (attn & (AFF_68881 | AFF_68882 | AFF_FPU40)) {
        return 1;
    }
    return 0;
}

static int load_host_has_fpu882(void)
{
    /* Strict 68882 bit; embedded 040/060 FPU is not tagged as .882. */
    if (SysBase->AttnFlags & AFF_68882) {
        return 1;
    }
    return 0;
}

static int load_host_has_mmu(void)
{
    UWORD attn;

    attn = SysBase->AttnFlags;
    if (attn & (AFF_68030 | AFF_68040 | AFF_68060)) {
        return 1;
    }
    return 0;
}

static int load_host_has_wos(void)
{
    if (FindName(&SysBase->LibList, (STRPTR)"ppc.library") != NULL) {
        return 1;
    }
    if (FindName(&SysBase->LibList, (STRPTR)"warp.library") != NULL) {
        return 1;
    }
    return 0;
}

static int load_host_has_pup(void)
{
    if (FindName(&SysBase->LibList, (STRPTR)"powerpc.library") != NULL) {
        return 1;
    }
    return 0;
}

int load_cpu_rank(STRPTR cpu)
{
    if (!cpu || !cpu[0]) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"any") == 0) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"wos") == 0 || Stricmp(cpu, (STRPTR)"pup") == 0) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"68000") == 0 || Stricmp(cpu, (STRPTR)"68010") == 0) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"68020") == 0) {
        return 20;
    }
    if (Stricmp(cpu, (STRPTR)"68030") == 0) {
        return 30;
    }
    if (Stricmp(cpu, (STRPTR)"68040") == 0) {
        return 40;
    }
    if (Stricmp(cpu, (STRPTR)"68060") == 0) {
        return 60;
    }
    return -1;
}

/*
 * 1 = host can run this variant; 0 = no.
 */
static int load_variant_runnable(const struct LoadFileVar *fv, int host_cpu)
{
    int rank;

    if (!fv) {
        return 0;
    }
    if (fv->wos) {
        return load_host_has_wos();
    }
    if (fv->pup) {
        return load_host_has_pup();
    }
    rank = load_cpu_rank(fv->cpu);
    if (rank < 0 || rank > host_cpu) {
        return 0;
    }
    if (fv->fpu882 && !load_host_has_fpu882()) {
        return 0;
    }
    if (fv->fpu881 && !fv->fpu882 && !load_host_has_fpu881()) {
        return 0;
    }
    if (fv->mmu && !load_host_has_mmu()) {
        return 0;
    }
    return 1;
}

/*
 * Higher is better among runnable candidates.
 * Prefer higher CPU; then FPU when host has FPU; then MMU when host has MMU.
 */
static int load_variant_score(const struct LoadFileVar *fv, int host_cpu)
{
    int score;
    int rank;

    rank = load_cpu_rank(fv->cpu);
    if (rank < 0) {
        rank = 0;
    }
    if (fv->wos || fv->pup) {
        score = 1000;
    } else {
        score = rank * 100;
    }
    if (fv->fpu882) {
        score += 30;
    } else if (fv->fpu881) {
        score += 20;
    } else if (load_host_has_fpu881()) {
        /* Prefer a no-FPU universal build less when FPU builds exist — score
         * without FPU stays lower so FPU variants win when runnable. */
        score += 0;
    }
    if (fv->mmu) {
        score += 5;
    }
    (void)host_cpu;
    return score;
}

LONG load_select_variants(struct LoadManifest *man)
{
    LONG i;
    LONG j;
    int host;
    int best_score;
    LONG best_idx;
    int score;
    int seen;

    if (!man) {
        return 0;
    }

    host = load_host_cpu_rank();

    for (i = 0; i < man->nfiles; i++) {
        man->files[i].selected = 0;
    }

    for (i = 0; i < man->nfiles; i++) {
        seen = 0;
        for (j = 0; j < i; j++) {
            if (Stricmp(man->files[j].dest, man->files[i].dest) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }

        best_score = -1;
        best_idx = -1;
        for (j = i; j < man->nfiles; j++) {
            if (Stricmp(man->files[j].dest, man->files[i].dest) != 0) {
                continue;
            }
            if (!load_variant_runnable(&man->files[j], host)) {
                continue;
            }
            score = load_variant_score(&man->files[j], host);
            if (score > best_score) {
                best_score = score;
                best_idx = j;
            }
        }
        if (best_idx < 0) {
            load_print_error((STRPTR)"no runnable variant for destination", 0);
            Printf("Load:   %s\n", (LONG)man->files[i].dest);
            Flush(Output());
            return 0;
        }
        man->files[best_idx].selected = 1;
    }
    return 1;
}

LONG load_check_deps(struct LoadManifest *man)
{
    ULONG need_maj;
    ULONG need_min;
    LONG i;
    struct Library *lib;
    ULONG got_maj;
    ULONG got_min;

    if (!man) {
        return 0;
    }

    /* OS: major.minor — compare against exec.library (SysBase). */
    if (man->os_min[0]
        && man->os_min[0] >= '0' && man->os_min[0] <= '9') {
        if (!load_ver_parse_dotted(man->os_min, &need_maj, &need_min)) {
            load_print_error((STRPTR)"bad OS: version in Manifest", 0);
            return 0;
        }
        got_maj = (ULONG)SysBase->LibNode.lib_Version;
        got_min = (ULONG)SysBase->LibNode.lib_Revision;
        if (got_maj < need_maj
            || (got_maj == need_maj && got_min < need_min)) {
            load_print_error((STRPTR)"OS version too old for package", 0);
            Printf("Load:   need exec %s, have %ld.%ld\n",
                (LONG)man->os_min, got_maj, got_min);
            Flush(Output());
            return 0;
        }
    }

    for (i = 0; i < man->nreqs; i++) {
        need_maj = 0;
        need_min = 0;
        if (man->reqs[i].minver[0]) {
            if (!load_ver_parse_dotted(man->reqs[i].minver, &need_maj,
                    &need_min)) {
                load_print_error((STRPTR)"bad Requires: Min= version", 0);
                Printf("Load:   %s\n", (LONG)man->reqs[i].name);
                Flush(Output());
                return 0;
            }
        }
        lib = OpenLibrary(man->reqs[i].name, need_maj);
        if (!lib) {
            load_print_error((STRPTR)"missing required library", 0);
            Printf("Load:   %s", (LONG)man->reqs[i].name);
            if (man->reqs[i].minver[0]) {
                Printf(" Min=%s", (LONG)man->reqs[i].minver);
            }
            Printf("\n", 0);
            if (man->reqs[i].url[0]) {
                Printf("Load:   obtain from %s\n", (LONG)man->reqs[i].url);
            }
            Flush(Output());
            return 0;
        }
        got_maj = (ULONG)lib->lib_Version;
        got_min = (ULONG)lib->lib_Revision;
        CloseLibrary(lib);
        if (got_maj < need_maj
            || (got_maj == need_maj && got_min < need_min)) {
            load_print_error((STRPTR)"required library too old", 0);
            Printf("Load:   %s need %s, have %ld.%ld\n",
                (LONG)man->reqs[i].name,
                (LONG)man->reqs[i].minver,
                got_maj, got_min);
            if (man->reqs[i].url[0]) {
                Printf("Load:   obtain from %s\n", (LONG)man->reqs[i].url);
            }
            Flush(Output());
            return 0;
        }
    }
    return 1;
}

static void load_skip_ws(STRPTR *pp)
{
    while (**pp == ' ' || **pp == '\t') {
        (*pp)++;
    }
}

static LONG load_copy_token(STRPTR dst, LONG dstlen, STRPTR *pp)
{
    LONG n;

    n = 0;
    while (**pp != '\0' && **pp != ' ' && **pp != '\t' && **pp != '\r'
        && **pp != '\n' && n < dstlen - 1) {
        dst[n++] = *(*pp)++;
    }
    dst[n] = '\0';
    return n > 0 ? 1 : 0;
}

static LONG load_parse_file_line(STRPTR line, struct LoadFileVar *out)
{
    STRPTR p;
    char tok[LOAD_VAL_LEN];

    out->dest[0] = '\0';
    out->from[0] = '\0';
    out->cpu[0] = '\0';
    out->fpu881 = 0;
    out->fpu882 = 0;
    out->mmu = 0;
    out->wos = 0;
    out->pup = 0;
    out->selected = 0;

    p = line;
    load_skip_ws(&p);
    if (Strnicmp(p, (STRPTR)"File:", 5) != 0) {
        return 0;
    }
    p += 5;
    load_skip_ws(&p);
    if (!load_copy_token(out->dest, LOAD_VAL_LEN, &p)) {
        return 0;
    }

    for (;;) {
        load_skip_ws(&p);
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
            break;
        }
        if (!load_copy_token(tok, LOAD_VAL_LEN, &p)) {
            break;
        }
        if (Strnicmp(tok, (STRPTR)"From=", 5) == 0) {
            Strncpy(out->from, tok + 5, LOAD_VAL_LEN);
        } else if (Strnicmp(tok, (STRPTR)"CPU=", 4) == 0) {
            Strncpy(out->cpu, tok + 4, (LONG)sizeof(out->cpu));
            if (Stricmp(out->cpu, (STRPTR)"wos") == 0) {
                out->wos = 1;
            } else if (Stricmp(out->cpu, (STRPTR)"pup") == 0) {
                out->pup = 1;
            }
        } else if (Strnicmp(tok, (STRPTR)"FPU=", 4) == 0) {
            if (Stricmp(tok + 4, (STRPTR)"882") == 0) {
                out->fpu882 = 1;
                out->fpu881 = 1;
            } else if (Stricmp(tok + 4, (STRPTR)"881") == 0) {
                out->fpu881 = 1;
            }
        } else if (Strnicmp(tok, (STRPTR)"MMU=", 4) == 0) {
            if (tok[4] == '1' || Stricmp(tok + 4, (STRPTR)"yes") == 0) {
                out->mmu = 1;
            }
        }
    }

    if (!out->from[0]) {
        return 0;
    }
    if (!out->cpu[0]) {
        Strncpy(out->cpu, (STRPTR)"68000", (LONG)sizeof(out->cpu));
    }
    return 1;
}

static LONG load_parse_requires_line(STRPTR line, struct LoadRequire *out)
{
    STRPTR p;
    char tok[LOAD_VAL_LEN];

    out->name[0] = '\0';
    out->minver[0] = '\0';
    out->url[0] = '\0';

    p = line;
    load_skip_ws(&p);
    if (Strnicmp(p, (STRPTR)"Requires:", 9) != 0) {
        return 0;
    }
    p += 9;
    load_skip_ws(&p);
    if (!load_copy_token(out->name, LOAD_VAL_LEN, &p)) {
        return 0;
    }
    for (;;) {
        load_skip_ws(&p);
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
            break;
        }
        if (!load_copy_token(tok, LOAD_VAL_LEN, &p)) {
            break;
        }
        if (Strnicmp(tok, (STRPTR)"Min=", 4) == 0) {
            Strncpy(out->minver, tok + 4, (LONG)sizeof(out->minver));
        } else if (Strnicmp(tok, (STRPTR)"URL=", 4) == 0) {
            Strncpy(out->url, tok + 4, LOAD_VAL_LEN);
        }
    }
    return 1;
}

static LONG load_parse_kv(STRPTR line, STRPTR key, LONG keylen,
    STRPTR val, LONG vallen)
{
    STRPTR p;
    LONG n;

    p = line;
    load_skip_ws(&p);
    if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') {
        return 0;
    }
    n = 0;
    while (*p != '\0' && *p != ':' && n < keylen - 1) {
        key[n++] = *p++;
    }
    key[n] = '\0';
    if (*p != ':') {
        return 0;
    }
    p++;
    load_skip_ws(&p);
    n = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n' && n < vallen - 1) {
        if (*p == '#' && (n == 0 || val[n - 1] == ' ')) {
            break;
        }
        val[n++] = *p++;
    }
    while (n > 0 && (val[n - 1] == ' ' || val[n - 1] == '\t')) {
        n--;
    }
    val[n] = '\0';
    return 1;
}

LONG load_parse_manifest(STRPTR text, LONG len, struct LoadManifest *man)
{
    LONG i;
    LONG start;
    char line[512];
    char key[LOAD_KEY_LEN];
    char val[LOAD_VAL_LEN];
    LONG llen;
    struct LoadFileVar fv;
    struct LoadRequire req;

    if (!text || !man || len < 0) {
        return 0;
    }

    man->name[0] = '\0';
    man->version[0] = '\0';
    man->os_min[0] = '\0';
    Strncpy(man->install_root, (STRPTR)"Work:", LOAD_VAL_LEN);
    man->nfiles = 0;
    man->nreqs = 0;

    i = 0;
    while (i < len) {
        start = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') {
            i++;
        }
        llen = i - start;
        if (llen >= (LONG)sizeof(line)) {
            llen = (LONG)sizeof(line) - 1;
        }
        if (llen > 0) {
            CopyMem(text + start, line, (ULONG)llen);
        }
        line[llen] = '\0';
        while (i < len && (text[i] == '\n' || text[i] == '\r')) {
            i++;
        }

        {
            STRPTR lp;

            lp = line;
            load_skip_ws(&lp);
            if (*lp == '#' || *lp == '\0') {
                continue;
            }
            if (Strnicmp(lp, (STRPTR)"File:", 5) == 0) {
                if (man->nfiles >= LOAD_MAX_FILES) {
                    load_print_error((STRPTR)"too many File: lines", 0);
                    return 0;
                }
                if (!load_parse_file_line(lp, &fv)) {
                    load_print_error((STRPTR)"bad File: line", 0);
                    return 0;
                }
                man->files[man->nfiles] = fv;
                man->nfiles++;
                continue;
            }
            if (Strnicmp(lp, (STRPTR)"Requires:", 9) == 0) {
                if (man->nreqs >= LOAD_MAX_REQS) {
                    load_print_error((STRPTR)"too many Requires: lines", 0);
                    return 0;
                }
                if (!load_parse_requires_line(lp, &req)) {
                    load_print_error((STRPTR)"bad Requires: line", 0);
                    return 0;
                }
                man->reqs[man->nreqs] = req;
                man->nreqs++;
                continue;
            }
            if (load_parse_kv(lp, key, LOAD_KEY_LEN, val, LOAD_VAL_LEN)) {
                if (Stricmp(key, (STRPTR)"Name") == 0) {
                    Strncpy(man->name, val, LOAD_VAL_LEN);
                } else if (Stricmp(key, (STRPTR)"Version") == 0) {
                    Strncpy(man->version, val, LOAD_VAL_LEN);
                } else if (Stricmp(key, (STRPTR)"InstallRoot") == 0) {
                    Strncpy(man->install_root, val, LOAD_VAL_LEN);
                } else if (Stricmp(key, (STRPTR)"OS") == 0) {
                    Strncpy(man->os_min, val, (LONG)sizeof(man->os_min));
                }
            }
        }
    }
    return 1;
}

static LONG load_dest_exists(STRPTR path)
{
    BPTR lock;

    lock = Lock(path, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
}

/*
 * Install one selected archive member with $VER gate and atomic place.
 * Returns: 1 ok, 0 fail, 2 skipped (not newer).
 */
static LONG load_install_one(struct LhArchive *arc, STRPTR from, STRPTR dest,
    struct LoadArgs *args, STRPTR man_version)
{
    APTR data;
    LONG len;
    UWORD expect_crc;
    char cand[LOAD_PATH_LEN];
    char token[LOAD_TOKEN_LEN];
    char bak[LOAD_PATH_LEN];
    struct LoadVerInfo cand_ver;
    struct LoadVerInfo inst_ver;
    ULONG man_maj;
    ULONG man_min;
    int cmp;
    LONG have_bak;

    if (load_path_is_volume_root(dest)) {
        load_print_error((STRPTR)"refusing to extract onto a volume root", 0);
        return 0;
    }

    len = LhReadData(arc, from, &data);
    if (len < 0 || !data) {
        load_print_error((STRPTR)"cannot read archive member", LhErr());
        Printf("Load:   %s\n", (LONG)from);
        Flush(Output());
        return 0;
    }
    expect_crc = load_crc16_buf(data, len);

    if (!load_ensure_tdir()) {
        FreeMem(data, (ULONG)len);
        load_print_error((STRPTR)"cannot create SYS:T/Load", IoErr());
        return 0;
    }
    if (!load_alloc_token((STRPTR)token, LOAD_TOKEN_LEN)) {
        FreeMem(data, (ULONG)len);
        load_print_error((STRPTR)"cannot allocate SYS:T/Load token", 0);
        return 0;
    }
    /* Candidate uses LHc + token (c = candidate). */
    {
        char leaf[16];
        LONG i;

        leaf[0] = 'L';
        leaf[1] = 'H';
        leaf[2] = 'c';
        for (i = 0; token[i] != '\0' && i < 8; i++) {
            leaf[3 + i] = token[i];
        }
        leaf[3 + i] = '\0';
        Strncpy((STRPTR)cand, (STRPTR)LOAD_TDIR, LOAD_PATH_LEN);
        if (!AddPart((STRPTR)cand, (STRPTR)leaf, LOAD_PATH_LEN)) {
            FreeMem(data, (ULONG)len);
            return 0;
        }
    }
    if (!load_write_file((STRPTR)cand, data, len)) {
        FreeMem(data, (ULONG)len);
        load_print_error((STRPTR)"cannot write candidate", IoErr());
        return 0;
    }
    FreeMem(data, (ULONG)len);
    data = NULL;

    {
        LONG vrc;

        vrc = load_ver_scan_file((STRPTR)cand, &cand_ver);
        if (vrc == 1 && cand_ver.have_ver) {
            /* ok */
        } else {
            DeleteFile((STRPTR)cand);
            if (vrc < 0) {
                load_print_error((STRPTR)"candidate has no $VER: tag", 0);
            } else {
                load_print_error(
                    (STRPTR)"candidate $VER: unparseable", 0);
            }
            Printf("Load:   %s (need $VER: name maj.min [(dd.mm.yy|dd/mm/yy)])\n",
                (LONG)from);
            Flush(Output());
            return 0;
        }
    }
    if (!man_version || !man_version[0]
        || !load_ver_parse_dotted(man_version, &man_maj, &man_min)
        || man_maj != cand_ver.major || man_min != cand_ver.minor) {
        DeleteFile((STRPTR)cand);
        load_print_error((STRPTR)"Manifest Version does not match $VER", 0);
        Printf("Load:   Manifest %s vs $VER %ld.%ld\n",
            (LONG)(man_version ? man_version : (STRPTR)"?"),
            cand_ver.major, cand_ver.minor);
        Flush(Output());
        return 0;
    }

    have_bak = 0;
    bak[0] = '\0';
    if (load_dest_exists(dest)) {
        {
            LONG vrc;

            vrc = load_ver_scan_file(dest, &inst_ver);
            if (vrc == 0) {
                DeleteFile((STRPTR)cand);
                load_print_error(
                    (STRPTR)"installed $VER: unparseable (fix file)",
                    0);
                Printf("Load:   %s\n", (LONG)dest);
                Flush(Output());
                return 0;
            }
            if (vrc < 0) {
                /* No $VER on installed file: treat as older / empty. */
                load_ver_clear(&inst_ver);
            }
        }
        cmp = load_ver_compare(&cand_ver, &inst_ver);
        if (cmp < 0 && !args->force) {
            DeleteFile((STRPTR)cand);
            if (!args->quiet) {
                Printf("Load: skip older %s\n", (LONG)dest);
                Flush(Output());
            }
            return 2;
        }
        if (cmp == 0 && !args->force) {
            DeleteFile((STRPTR)cand);
            if (!args->quiet) {
                Printf("Load: skip same %s\n", (LONG)dest);
                Flush(Output());
            }
            return 2;
        }
        if (!load_backup_to_t(dest, (STRPTR)token, LOAD_TOKEN_LEN,
                (STRPTR)bak, LOAD_PATH_LEN)) {
            DeleteFile((STRPTR)cand);
            load_print_error((STRPTR)"cannot backup existing file to SYS:T/Load",
                IoErr());
            return 0;
        }
        have_bak = 1;
        if (!args->quiet) {
            Printf("Load: bak %s\n", (LONG)bak);
            Flush(Output());
        }
    }

    if (!args->quiet) {
        Printf("Load: %s -> %s\n", (LONG)from, (LONG)dest);
        Flush(Output());
    }

    if (!load_place_file((STRPTR)cand, dest, expect_crc,
            have_bak ? (STRPTR)bak : NULL)) {
        DeleteFile((STRPTR)cand);
        return 0;
    }
    DeleteFile((STRPTR)cand);
    if (have_bak && !args->quiet) {
        Printf("Load: rollback copy kept at %s\n", (LONG)bak);
        Flush(Output());
    }
    return 1;
}

static void load_print_readme(struct LhArchive *arc, STRPTR pkg_name)
{
    char member[LOAD_VAL_LEN];
    APTR data;
    LONG len;
    LONG L;

    if (!arc || !pkg_name || !pkg_name[0]) {
        return;
    }
    Strncpy((STRPTR)member, pkg_name, LOAD_VAL_LEN);
    L = 0;
    while (member[L] != '\0' && L < LOAD_VAL_LEN - 1) {
        L++;
    }
    if (L + 8 >= LOAD_VAL_LEN) {
        return;
    }
    member[L++] = '.';
    member[L++] = 'r';
    member[L++] = 'e';
    member[L++] = 'a';
    member[L++] = 'd';
    member[L++] = 'm';
    member[L++] = 'e';
    member[L] = '\0';

    data = NULL;
    len = LhReadData(arc, (STRPTR)member, &data);
    if (len <= 0 || !data) {
        return;
    }
    Printf("\n", 0);
    Flush(Output());
    Write(Output(), data, len);
    if (len > 0 && ((UBYTE *)data)[len - 1] != '\n') {
        Printf("\n", 0);
    }
    Printf("\n", 0);
    Flush(Output());
    FreeMem(data, (ULONG)len);
}

#define LOAD_SUMMARY_MAX 24
#define LOAD_SUMMARY_LEN 128

LONG load_run(struct LoadArgs *args)
{
    struct LhArchive *arc;
    APTR man_data;
    LONG man_len;
    struct LoadManifest man;
    LONG i;
    char dest[LOAD_PATH_LEN];
    LONG rc;
    LONG one;
    char installed[LOAD_SUMMARY_MAX][LOAD_SUMMARY_LEN];
    char skipped[LOAD_SUMMARY_MAX][LOAD_SUMMARY_LEN];
    LONG ninst;
    LONG nskip;

    ninst = 0;
    nskip = 0;

    if (args->rollback) {
        if (!args->dest || !args->dest[0]) {
            load_print_error((STRPTR)"ROLLBACK requires DEST", 0);
            return RETURN_FAIL;
        }
        return load_rollback_dest(args->dest, args->quiet);
    }

    arc = LhOpenArchive(args->bin, LHARC_MODE_READ);
    if (!arc) {
        load_print_error((STRPTR)"cannot open Bin", LhErr());
        return RETURN_FAIL;
    }

    man_data = NULL;
    man_len = LhReadData(arc, (STRPTR)"Manifest", &man_data);
    if (man_len < 0 || !man_data) {
        LhCloseArchive(arc);
        load_print_error((STRPTR)"cannot read Manifest", LhErr());
        return RETURN_FAIL;
    }

    if (!load_parse_manifest((STRPTR)man_data, man_len, &man)) {
        FreeMem(man_data, (ULONG)man_len);
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }
    FreeMem(man_data, (ULONG)man_len);
    man_data = NULL;

    if (!args->quiet) {
        if (man.name[0]) {
            Printf("Load: package %s", (LONG)man.name);
            if (man.version[0]) {
                Printf(" %s", (LONG)man.version);
            }
            Printf("\n", 0);
            Flush(Output());
        }
    }

    if (man.nfiles == 0) {
        LhCloseArchive(arc);
        load_print_error((STRPTR)"Manifest has no File: map", 0);
        return RETURN_FAIL;
    }

    if (!man.version[0]) {
        LhCloseArchive(arc);
        load_print_error((STRPTR)"Manifest missing Version", 0);
        return RETURN_FAIL;
    }

    if (!load_check_deps(&man)) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }

    if (!load_select_variants(&man)) {
        LhCloseArchive(arc);
        return RETURN_FAIL;
    }

    rc = RETURN_OK;
    for (i = 0; i < man.nfiles; i++) {
        if (!man.files[i].selected) {
            continue;
        }
        if (!load_resolve_dest(dest, LOAD_PATH_LEN, man.files[i].dest,
            args->to, man.install_root)) {
            load_print_error((STRPTR)"cannot resolve destination", 0);
            LhCloseArchive(arc);
            return RETURN_FAIL;
        }
        one = load_install_one(arc, man.files[i].from, dest, args,
            man.version);
        if (one == 0) {
            LhCloseArchive(arc);
            return RETURN_FAIL;
        }
        if (one == 2) {
            if (nskip < LOAD_SUMMARY_MAX) {
                Strncpy(skipped[nskip], dest, LOAD_SUMMARY_LEN);
                nskip++;
            }
        } else if (ninst < LOAD_SUMMARY_MAX) {
            Strncpy(installed[ninst], dest, LOAD_SUMMARY_LEN);
            ninst++;
        }
    }

    if (!args->quiet) {
        load_print_readme(arc, man.name);
        Printf("Load: summary\n", 0);
        if (ninst == 0 && nskip == 0) {
            Printf("  (nothing selected)\n", 0);
        }
        for (i = 0; i < ninst; i++) {
            Printf("  installed  %s\n", (LONG)installed[i]);
        }
        for (i = 0; i < nskip; i++) {
            Printf("  skipped    %s\n", (LONG)skipped[i]);
        }
        Printf("Load: done\n", 0);
        Flush(Output());
    }

    LhCloseArchive(arc);
    return rc;
}
