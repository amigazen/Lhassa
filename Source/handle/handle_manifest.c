/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_manifest.c - Scan stage and generate Manifest text.
 *
 * System mode (FROM directory named SYS):
 *   FROM/Libs/foo.library
 *   FROM/Libs/foo.library.020.881
 *   FROM/Manifest   (optional: Summary, OS, Requires)
 *
 * Application mode (any other FROM name):
 *   FROM/MyApp, FROM/data/..., optional FROM/Manifest
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "handle_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

static void handle_str_cat(STRPTR dst, LONG dstlen, STRPTR src)
{
    LONG L;
    LONG i;

    if (!dst || dstlen <= 0 || !src) {
        return;
    }
    L = 0;
    while (dst[L] != '\0' && L < dstlen - 1) {
        L++;
    }
    i = 0;
    while (src[i] != '\0' && L < dstlen - 1) {
        dst[L++] = src[i++];
    }
    dst[L] = '\0';
}

static void handle_append_ulong(STRPTR dst, LONG dstlen, ULONG v)
{
    char tmp[12];
    LONG n;
    LONG i;
    LONG L;

    n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < 11) {
            tmp[n++] = (char)('0' + (v % 10UL));
            v /= 10UL;
        }
    }
    L = 0;
    while (dst[L] != '\0' && L < dstlen - 1) {
        L++;
    }
    for (i = n - 1; i >= 0 && L < dstlen - 1; i--) {
        dst[L++] = tmp[i];
    }
    dst[L] = '\0';
}

/*
 * Map SYS/<stem>/... to assign stem (LIBS, Devs, CLASSES, ...).
 * Returns 0 if stem is not a known SYS subdirectory.
 */
static LONG handle_sys_stem_assign(STRPTR stem, STRPTR out, LONG outlen)
{
    if (!stem || !out || outlen <= 0) {
        return 0;
    }
    if (Stricmp(stem, (STRPTR)"Libs") == 0
        || Stricmp(stem, (STRPTR)"LIBS") == 0) {
        Strncpy(out, (STRPTR)"LIBS", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"C") == 0) {
        Strncpy(out, (STRPTR)"C", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"Devs") == 0
        || Stricmp(stem, (STRPTR)"DEVS") == 0) {
        Strncpy(out, (STRPTR)"Devs", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"L") == 0) {
        Strncpy(out, (STRPTR)"L", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"Prefs") == 0) {
        Strncpy(out, (STRPTR)"Prefs", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"Fonts") == 0) {
        Strncpy(out, (STRPTR)"Fonts", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"Expansion") == 0) {
        Strncpy(out, (STRPTR)"Expansion", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"Locale") == 0) {
        Strncpy(out, (STRPTR)"Locale", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"System") == 0) {
        Strncpy(out, (STRPTR)"System", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"S") == 0) {
        Strncpy(out, (STRPTR)"S", outlen);
        return 1;
    }
    if (Stricmp(stem, (STRPTR)"Classes") == 0
        || Stricmp(stem, (STRPTR)"CLASSES") == 0) {
        Strncpy(out, (STRPTR)"CLASSES", outlen);
        return 1;
    }
    return 0;
}

static LONG handle_add_file(struct HandleScan *scan, STRPTR dest,
    STRPTR from, const struct HandleVariant *v)
{
    struct HandleFileVar *fv;

    if (scan->nfiles >= HANDLE_MAX_FILES) {
        handle_print_error((STRPTR)"too many staged files", 0);
        return 0;
    }
    fv = &scan->files[scan->nfiles];
    Strncpy(fv->dest, dest, HANDLE_VAL_LEN);
    Strncpy(fv->from, from, HANDLE_VAL_LEN);
    handle_variant_cpu_string(v, fv->cpu, (LONG)sizeof(fv->cpu));
    fv->fpu881 = v->fpu881;
    fv->fpu882 = v->fpu882;
    fv->mmu = v->mmu;
    fv->wos = v->wos;
    fv->pup = v->pup;
    scan->nfiles++;
    return 1;
}

/*
 * Walk under SYS/<stem>/... ; dest_prefix is like LIBS: or LIBS:subdir/
 * arc_rel is SYS/Libs/...
 */
static LONG handle_walk_sys_tree(struct HandleScan *scan, BPTR dir_lock,
    STRPTR disk_dir, STRPTR arc_rel, STRPTR dest_prefix)
{
    struct FileInfoBlock *fib;
    char child_disk[HANDLE_PATH_LEN];
    char child_arc[HANDLE_PATH_LEN];
    char child_dest[HANDLE_VAL_LEN];
    char base_leaf[HANDLE_VAL_LEN];
    struct HandleVariant var;
    BPTR child_lock;

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        return 0;
    }
    if (!Examine(dir_lock, fib) || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    while (ExNext(dir_lock, fib)) {
        if (handle_is_hidden_name(fib->fib_FileName)) {
            continue;
        }
        if (!handle_path_join((STRPTR)child_disk, HANDLE_PATH_LEN, disk_dir,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_arc_join((STRPTR)child_arc, HANDLE_PATH_LEN, arc_rel,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }

        if (fib->fib_DirEntryType > 0) {
            Strncpy((STRPTR)child_dest, dest_prefix, HANDLE_VAL_LEN);
            handle_str_cat((STRPTR)child_dest, HANDLE_VAL_LEN,
                fib->fib_FileName);
            handle_str_cat((STRPTR)child_dest, HANDLE_VAL_LEN, (STRPTR)"/");
            child_lock = Lock((STRPTR)child_disk, ACCESS_READ);
            if (!child_lock) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            if (!handle_walk_sys_tree(scan, child_lock, (STRPTR)child_disk,
                    (STRPTR)child_arc, (STRPTR)child_dest)) {
                UnLock(child_lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            UnLock(child_lock);
        } else {
            if (!handle_variant_parse_leaf(fib->fib_FileName,
                    (STRPTR)base_leaf, HANDLE_VAL_LEN, &var)) {
                handle_print_error(
                    (STRPTR)"unknown or bad secondary extension on staged file",
                    0);
                Printf("Handle:   %s\n", (LONG)child_arc);
                Printf("Handle:   allowed: .020 .030 .040 .060 .881 .882 .mmu .wos .pup\n",
                    0);
                Flush(Output());
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            if (var.wos && var.pup) {
                handle_print_error((STRPTR)".wos and .pup cannot combine", 0);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            Strncpy((STRPTR)child_dest, dest_prefix, HANDLE_VAL_LEN);
            handle_str_cat((STRPTR)child_dest, HANDLE_VAL_LEN,
                (STRPTR)base_leaf);
            if (!handle_add_file(scan, (STRPTR)child_dest, (STRPTR)child_arc,
                    &var)) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
        }
    }
    if (IoErr() != ERROR_NO_MORE_ENTRIES) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    return 1;
}

static LONG handle_walk_sys_root(struct HandleScan *scan, BPTR sys_lock,
    STRPTR disk_sys, STRPTR arc_sys)
{
    struct FileInfoBlock *fib;
    char child_disk[HANDLE_PATH_LEN];
    char arc_rel[HANDLE_PATH_LEN];
    char assign[32];
    char dest_prefix[HANDLE_VAL_LEN];
    BPTR child_lock;

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        return 0;
    }
    if (!Examine(sys_lock, fib) || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    while (ExNext(sys_lock, fib)) {
        if (fib->fib_DirEntryType <= 0) {
            continue;
        }
        if (handle_is_hidden_name(fib->fib_FileName)) {
            continue;
        }
        if (!handle_sys_stem_assign(fib->fib_FileName, (STRPTR)assign,
                (LONG)sizeof(assign))) {
            continue;
        }
        if (!handle_path_join((STRPTR)child_disk, HANDLE_PATH_LEN, disk_sys,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_arc_join((STRPTR)arc_rel, HANDLE_PATH_LEN, arc_sys,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        Strncpy((STRPTR)dest_prefix, (STRPTR)assign, HANDLE_VAL_LEN);
        handle_str_cat((STRPTR)dest_prefix, HANDLE_VAL_LEN, (STRPTR)":");

        child_lock = Lock((STRPTR)child_disk, ACCESS_READ);
        if (!child_lock) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_walk_sys_tree(scan, child_lock, (STRPTR)child_disk,
                (STRPTR)arc_rel, (STRPTR)dest_prefix)) {
            UnLock(child_lock);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        UnLock(child_lock);
    }
    if (IoErr() != ERROR_NO_MORE_ENTRIES) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    return 1;
}

/*
 * Join FROM root with archive-style relative path (slash-separated).
 */
static LONG handle_disk_from_rel(STRPTR out, LONG outlen, STRPTR root,
    STRPTR rel)
{
    char seg[HANDLE_VAL_LEN];
    LONG i;
    LONG j;

    if (!out || outlen <= 0 || !root || !rel) {
        return 0;
    }
    Strncpy(out, root, outlen);
    i = 0;
    while (rel[i] != '\0') {
        while (rel[i] == '/') {
            i++;
        }
        if (rel[i] == '\0') {
            break;
        }
        j = 0;
        while (rel[i] != '\0' && rel[i] != '/' && j < (LONG)sizeof(seg) - 1) {
            seg[j++] = rel[i++];
        }
        seg[j] = '\0';
        if (!AddPart(out, (STRPTR)seg, (ULONG)outlen)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Map archive member path to disk under FROM.
 * System archives use a leading SYS/ prefix; disk paths omit it.
 */
static LONG handle_disk_from_arc(STRPTR out, LONG outlen, STRPTR from_root,
    STRPTR arc_rel, ULONG sys_mode)
{
    STRPTR rel;

    rel = arc_rel;
    if (sys_mode && arc_rel && Strnicmp(arc_rel, (STRPTR)"SYS/", 4) == 0) {
        rel = arc_rel + 4;
    } else if (sys_mode && arc_rel && Stricmp(arc_rel, (STRPTR)"SYS") == 0) {
        rel = (STRPTR)"";
    }
    return handle_disk_from_rel(out, outlen, from_root, rel);
}

/*
 * Walk application tree: archive and install paths mirror FROM layout.
 */
static LONG handle_walk_app_tree(struct HandleScan *scan, BPTR dir_lock,
    STRPTR disk_dir, STRPTR arc_rel)
{
    struct FileInfoBlock *fib;
    char child_disk[HANDLE_PATH_LEN];
    char child_arc[HANDLE_PATH_LEN];
    char child_dest[HANDLE_VAL_LEN];
    char base_leaf[HANDLE_VAL_LEN];
    struct HandleVariant var;
    BPTR child_lock;

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        return 0;
    }
    if (!Examine(dir_lock, fib) || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    while (ExNext(dir_lock, fib)) {
        if (handle_is_hidden_name(fib->fib_FileName)) {
            continue;
        }
        if (Stricmp(fib->fib_FileName, (STRPTR)"Manifest") == 0
            && arc_rel[0] == '\0') {
            continue;
        }
        if (!handle_path_join((STRPTR)child_disk, HANDLE_PATH_LEN, disk_dir,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_arc_join((STRPTR)child_arc, HANDLE_PATH_LEN, arc_rel,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }

        if (fib->fib_DirEntryType > 0) {
            child_lock = Lock((STRPTR)child_disk, ACCESS_READ);
            if (!child_lock) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            if (!handle_walk_app_tree(scan, child_lock, (STRPTR)child_disk,
                    (STRPTR)child_arc)) {
                UnLock(child_lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            UnLock(child_lock);
        } else {
            if (!handle_variant_parse_leaf(fib->fib_FileName,
                    (STRPTR)base_leaf, HANDLE_VAL_LEN, &var)) {
                handle_print_error(
                    (STRPTR)"unknown or bad secondary extension on staged file",
                    0);
                Printf("Handle:   %s\n", (LONG)child_arc);
                Printf("Handle:   allowed: .020 .030 .040 .060 .881 .882 .mmu .wos .pup\n",
                    0);
                Flush(Output());
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            if (var.wos && var.pup) {
                handle_print_error((STRPTR)".wos and .pup cannot combine", 0);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            if (arc_rel[0] != '\0') {
                if (!handle_arc_join((STRPTR)child_dest, HANDLE_VAL_LEN,
                        arc_rel, (STRPTR)base_leaf)) {
                    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                    return 0;
                }
            } else {
                Strncpy((STRPTR)child_dest, (STRPTR)base_leaf, HANDLE_VAL_LEN);
            }
            if (!handle_add_file(scan, (STRPTR)child_dest, (STRPTR)child_arc,
                    &var)) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
        }
    }
    if (IoErr() != ERROR_NO_MORE_ENTRIES) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    return 1;
}

static LONG handle_path_has_colon(STRPTR path)
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

static LONG handle_primary_disk_path(STRPTR out, LONG outlen, STRPTR from_root,
    STRPTR primary_rel)
{
    char seg[HANDLE_VAL_LEN];
    LONG i;
    LONG j;

    if (!out || outlen <= 0 || !from_root || !primary_rel || !primary_rel[0]) {
        return 0;
    }
    Strncpy(out, from_root, outlen);
    i = 0;
    while (primary_rel[i] != '\0') {
        while (primary_rel[i] == '/') {
            i++;
        }
        if (primary_rel[i] == '\0') {
            break;
        }
        j = 0;
        while (primary_rel[i] != '\0' && primary_rel[i] != '/'
            && j < (LONG)sizeof(seg) - 1) {
            seg[j++] = primary_rel[i++];
        }
        seg[j] = '\0';
        if (!AddPart(out, (STRPTR)seg, (ULONG)outlen)) {
            return 0;
        }
    }
    return 1;
}

/*
 * Resolve PRIMARY to a disk path.
 * System mode: PRIMARY=Libs/foo.library or PRIMARY=SYS/Libs/foo.library
 * (leading SYS/ is stripped when FROM is the SYS directory).
 * Also accepts an absolute Amiga path, or a path relative to the cwd.
 */
static LONG handle_resolve_primary(struct HandleArgs *args, STRPTR from_root,
    STRPTR out, LONG outlen)
{
    STRPTR rel;
    BPTR lock;

    if (!args || !args->primary || !args->primary[0] || !out || outlen <= 0) {
        return 0;
    }

    if (handle_path_has_colon(args->primary)) {
        Strncpy(out, args->primary, outlen);
        lock = Lock(out, ACCESS_READ);
        if (lock) {
            UnLock(lock);
            return 1;
        }
        return 0;
    }

    rel = args->primary;
    if (args->sys_mode && Strnicmp(rel, (STRPTR)"SYS/", 4) == 0) {
        rel = rel + 4;
    }

    if (rel[0] != '\0'
        && handle_primary_disk_path(out, outlen, from_root, rel)) {
        lock = Lock(out, ACCESS_READ);
        if (lock) {
            UnLock(lock);
            return 1;
        }
    }

    /* Fallback: PRIMARY as typed from the current directory. */
    Strncpy(out, args->primary, outlen);
    lock = Lock(out, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
}

static LONG handle_readme_leaf(struct HandleArgs *args, STRPTR leaf, LONG leaflen)
{
    if (!args || !args->name || !args->name[0] || !leaf || leaflen < 8) {
        return 0;
    }
    Strncpy(leaf, args->name, leaflen);
    handle_str_cat(leaf, leaflen, (STRPTR)".readme");
    return 1;
}

static LONG handle_try_readme_path(STRPTR path, STRPTR disk_out, LONG outlen)
{
    BPTR lock;

    if (!path || !path[0] || !disk_out || outlen <= 0) {
        return 0;
    }
    lock = Lock(path, ACCESS_READ);
    if (!lock) {
        return 0;
    }
    UnLock(lock);
    Strncpy(disk_out, path, outlen);
    return 1;
}

LONG handle_find_readme(struct HandleArgs *args, STRPTR disk_out, LONG outlen,
    STRPTR arc_out, LONG arclen)
{
    char leaf[HANDLE_VAL_LEN];
    char try_path[HANDLE_PATH_LEN];
    char parent[HANDLE_PATH_LEN];
    STRPTR slash;
    LONG i;

    if (!handle_readme_leaf(args, (STRPTR)leaf, HANDLE_VAL_LEN)) {
        return 0;
    }
    if (arc_out && arclen > 0) {
        Strncpy(arc_out, (STRPTR)leaf, arclen);
    }

    /* 1) Current directory. */
    if (handle_try_readme_path((STRPTR)leaf, disk_out, outlen)) {
        return 1;
    }

    /* 2) Inside FROM (e.g. SYS/ttengine.readme). */
    if (handle_path_join((STRPTR)try_path, HANDLE_PATH_LEN, args->from_path,
            (STRPTR)leaf)
        && handle_try_readme_path((STRPTR)try_path, disk_out, outlen)) {
        return 1;
    }

    /* 3) Parent of FROM (stage beside SYS/). */
    Strncpy((STRPTR)parent, args->from_path, HANDLE_PATH_LEN);
    slash = NULL;
    for (i = 0; parent[i] != '\0'; i++) {
        if (parent[i] == '/' || parent[i] == ':') {
            slash = &parent[i];
        }
    }
    if (slash && slash[0] == '/') {
        *slash = '\0';
        if (parent[0] != '\0'
            && handle_path_join((STRPTR)try_path, HANDLE_PATH_LEN,
                (STRPTR)parent, (STRPTR)leaf)
            && handle_try_readme_path((STRPTR)try_path, disk_out, outlen)) {
            return 1;
        }
    } else if (slash && slash[0] == ':' && slash[1] != '\0') {
        /* Volume:subdir/SYS → Volume:subdir/NAME.readme */
        slash[1] = '\0';
        if (handle_path_join((STRPTR)try_path, HANDLE_PATH_LEN, (STRPTR)parent,
                (STRPTR)leaf)
            && handle_try_readme_path((STRPTR)try_path, disk_out, outlen)) {
            return 1;
        }
    }

    return 0;
}

static LONG handle_set_package_version(struct HandleArgs *args,
    struct HandleScan *scan, STRPTR from_root, ULONG *pkg_maj, ULONG *pkg_min)
{
    char primary_disk[HANDLE_PATH_LEN];
    struct HandleVerInfo pvi;

    if (pkg_maj) {
        *pkg_maj = 0;
    }
    if (pkg_min) {
        *pkg_min = 0;
    }

    if (args->version && args->version[0]) {
        Strncpy(scan->version, args->version, HANDLE_VAL_LEN);
        if (pkg_maj && pkg_min
            && !handle_ver_parse_dotted(args->version, pkg_maj, pkg_min)) {
            handle_print_error((STRPTR)"bad VERSION= (need major.minor)", 0);
            return 0;
        }
        handle_ver_clear(&scan->pkg_ver);
        scan->pkg_ver.have_ver = 1;
        if (pkg_maj) {
            scan->pkg_ver.major = *pkg_maj;
        }
        if (pkg_min) {
            scan->pkg_ver.minor = *pkg_min;
        }
        return 1;
    }

    if (!handle_resolve_primary(args, from_root, (STRPTR)primary_disk,
            HANDLE_PATH_LEN)) {
        handle_print_error((STRPTR)"PRIMARY file not found", 0);
        Printf("Handle:   tried %s (under FROM, or as given)\n",
            (LONG)args->primary);
        Flush(Output());
        return 0;
    }
    if (!handle_ver_scan_file((STRPTR)primary_disk, &pvi)) {
        handle_print_error((STRPTR)"PRIMARY missing or unparseable $VER:", 0);
        Printf("Handle:   %s\n", (LONG)primary_disk);
        Printf("Handle:   need $VER: name maj.min [(dd.mm.yy|dd/mm/yy)]\n", 0);
        Flush(Output());
        return 0;
    }
    scan->version[0] = '\0';
    handle_append_ulong(scan->version, HANDLE_VAL_LEN, pvi.major);
    handle_str_cat(scan->version, HANDLE_VAL_LEN, (STRPTR)".");
    handle_append_ulong(scan->version, HANDLE_VAL_LEN, pvi.minor);
    scan->pkg_ver = pvi;
    if (pkg_maj) {
        *pkg_maj = pvi.major;
    }
    if (pkg_min) {
        *pkg_min = pvi.minor;
    }
    return 1;
}

static LONG handle_check_sys_ver_uniform(struct HandleScan *scan,
    STRPTR from_root, ULONG pkg_maj, ULONG pkg_min)
{
    LONG i;
    struct HandleVerInfo vi;
    char disk_path[HANDLE_PATH_LEN];

    for (i = 0; i < scan->nfiles; i++) {
        if (!handle_disk_from_arc((STRPTR)disk_path, HANDLE_PATH_LEN,
                from_root, scan->files[i].from, scan->sys_mode)) {
            return 0;
        }
        if (!handle_ver_scan_file((STRPTR)disk_path, &vi)) {
            handle_print_error(
                (STRPTR)"staged file missing or unparseable $VER:", 0);
            Printf("Handle:   %s\n", (LONG)disk_path);
            Printf("Handle:   need $VER: name maj.min [(dd.mm.yy|dd/mm/yy)]\n",
                0);
            Flush(Output());
            return 0;
        }
        if (vi.major != pkg_maj || vi.minor != pkg_min) {
            handle_print_error(
                (STRPTR)"binary $VER does not match PRIMARY package version",
                0);
            Printf("Handle:   %s is %ld.%ld, package is %ld.%ld\n",
                (LONG)disk_path, vi.major, vi.minor, pkg_maj, pkg_min);
            Flush(Output());
            return 0;
        }
    }
    return 1;
}

static void handle_skip_ws(STRPTR *pp)
{
    while (**pp == ' ' || **pp == '\t') {
        (*pp)++;
    }
}

static LONG handle_copy_token(STRPTR dst, LONG dstlen, STRPTR *pp)
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

static LONG handle_parse_kv(STRPTR line, STRPTR key, LONG keylen,
    STRPTR val, LONG vallen)
{
    STRPTR p;
    LONG n;

    p = line;
    handle_skip_ws(&p);
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
    handle_skip_ws(&p);
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

static LONG handle_parse_requires_line(STRPTR line, struct HandleRequire *out)
{
    STRPTR p;
    char tok[HANDLE_VAL_LEN];

    out->name[0] = '\0';
    out->minver[0] = '\0';
    out->url[0] = '\0';

    p = line;
    handle_skip_ws(&p);
    if (Strnicmp(p, (STRPTR)"Requires:", 9) != 0) {
        return 0;
    }
    p += 9;
    handle_skip_ws(&p);
    if (!handle_copy_token(out->name, HANDLE_VAL_LEN, &p)) {
        return 0;
    }
    for (;;) {
        handle_skip_ws(&p);
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
            break;
        }
        if (!handle_copy_token(tok, HANDLE_VAL_LEN, &p)) {
            break;
        }
        if (Strnicmp(tok, (STRPTR)"Min=", 4) == 0) {
            Strncpy(out->minver, tok + 4, (LONG)sizeof(out->minver));
        } else if (Strnicmp(tok, (STRPTR)"URL=", 4) == 0) {
            Strncpy(out->url, tok + 4, HANDLE_VAL_LEN);
        }
    }
    return 1;
}

/*
 * Optional staged Manifest: Summary/OS/Requires only (not File:/Version/Name).
 */
static LONG handle_merge_stage_manifest(STRPTR from_root, struct HandleScan *scan)
{
    char path[HANDLE_PATH_LEN];
    BPTR fh;
    char line[512];
    char key[64];
    char val[HANDLE_VAL_LEN];
    LONG n;
    UBYTE ch;
    LONG llen;

    if (!handle_path_join((STRPTR)path, HANDLE_PATH_LEN, from_root,
            (STRPTR)"Manifest")) {
        return 1;
    }
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        return 1;
    }

    llen = 0;
    for (;;) {
        n = Read(fh, &ch, 1);
        if (n < 1) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            if (llen > 0) {
                line[llen] = '\0';
                {
                    STRPTR lp;

                    lp = line;
                    handle_skip_ws(&lp);
                    if (*lp != '#' && *lp != '\0') {
                        if (Strnicmp(lp, (STRPTR)"Requires:", 9) == 0) {
                            if (scan->nreqs >= HANDLE_MAX_REQS) {
                                Close(fh);
                                handle_print_error(
                                    (STRPTR)"too many Requires: lines", 0);
                                return 0;
                            }
                            if (!handle_parse_requires_line(lp,
                                    &scan->reqs[scan->nreqs])) {
                                Close(fh);
                                handle_print_error(
                                    (STRPTR)"bad Requires: line in staged Manifest",
                                    0);
                                return 0;
                            }
                            scan->nreqs++;
                        } else if (handle_parse_kv(lp, key, (LONG)sizeof(key),
                                val, HANDLE_VAL_LEN)) {
                            if (Stricmp(key, (STRPTR)"Summary") == 0) {
                                Strncpy(scan->summary, val, HANDLE_VAL_LEN);
                            } else if (Stricmp(key, (STRPTR)"OS") == 0) {
                                Strncpy(scan->os_min, val, (LONG)sizeof(scan->os_min));
                            }
                        }
                    }
                }
            }
            llen = 0;
            continue;
        }
        if (llen < (LONG)sizeof(line) - 1) {
            line[llen++] = (char)ch;
        }
    }
    if (llen > 0) {
        line[llen] = '\0';
        {
            STRPTR lp;

            lp = line;
            handle_skip_ws(&lp);
            if (*lp != '#' && *lp != '\0') {
                if (Strnicmp(lp, (STRPTR)"Requires:", 9) == 0) {
                    if (scan->nreqs < HANDLE_MAX_REQS
                        && handle_parse_requires_line(lp,
                            &scan->reqs[scan->nreqs])) {
                        scan->nreqs++;
                    }
                } else if (handle_parse_kv(lp, key, (LONG)sizeof(key), val,
                        HANDLE_VAL_LEN)) {
                    if (Stricmp(key, (STRPTR)"Summary") == 0) {
                        Strncpy(scan->summary, val, HANDLE_VAL_LEN);
                    } else if (Stricmp(key, (STRPTR)"OS") == 0) {
                        Strncpy(scan->os_min, val,
                            (LONG)sizeof(scan->os_min));
                    }
                }
            }
        }
    }
    Close(fh);
    return 1;
}

LONG handle_scan_stage(struct HandleArgs *args, struct HandleScan *scan)
{
    BPTR from_lock;
    ULONG pkg_maj;
    ULONG pkg_min;

    if (!args || !scan || !args->from_path[0]) {
        return 0;
    }

    scan->sys_mode = args->sys_mode;
    scan->name[0] = '\0';
    scan->version[0] = '\0';
    scan->os_min[0] = '\0';
    scan->summary[0] = '\0';
    scan->nfiles = 0;
    scan->nreqs = 0;
    handle_ver_clear(&scan->pkg_ver);

    Strncpy(scan->name, args->name, HANDLE_VAL_LEN);

    from_lock = Lock(args->from_path, ACCESS_READ);
    if (!from_lock) {
        handle_print_error((STRPTR)"cannot lock FROM", IoErr());
        return 0;
    }

    if (args->sys_mode) {
        if (!handle_walk_sys_root(scan, from_lock, args->from_path,
                (STRPTR)"SYS")) {
            UnLock(from_lock);
            return 0;
        }
        if (scan->nfiles == 0) {
            handle_print_error((STRPTR)"no files under SYS/", 0);
            Printf("Handle: expected Libs/, Devs/, Classes/, ... under FROM\n",
                0);
            Flush(Output());
            UnLock(from_lock);
            return 0;
        }
    } else {
        if (!handle_walk_app_tree(scan, from_lock, args->from_path,
                (STRPTR)"")) {
            UnLock(from_lock);
            return 0;
        }
        if (scan->nfiles == 0) {
            handle_print_error((STRPTR)"no files under FROM", 0);
            UnLock(from_lock);
            return 0;
        }
    }
    UnLock(from_lock);

    if (!handle_merge_stage_manifest(args->from_path, scan)) {
        return 0;
    }

    if (!scan->summary[0]) {
        Strncpy(scan->summary, (STRPTR)"generated by Handle", HANDLE_VAL_LEN);
    }

    pkg_maj = 0;
    pkg_min = 0;
    if (!handle_set_package_version(args, scan, args->from_path,
            &pkg_maj, &pkg_min)) {
        return 0;
    }

    if (args->sys_mode) {
        if (!handle_check_sys_ver_uniform(scan, args->from_path,
                pkg_maj, pkg_min)) {
            return 0;
        }
    }

    return 1;
}

LONG handle_build_manifest(struct HandleScan *scan, STRPTR out, LONG outlen)
{
    LONG i;

    if (!scan || !out || outlen < 64) {
        return 0;
    }
    out[0] = '\0';
    handle_str_cat(out, outlen, (STRPTR)"Format: 1\n");
    handle_str_cat(out, outlen, (STRPTR)"Name: ");
    handle_str_cat(out, outlen, scan->name);
    handle_str_cat(out, outlen, (STRPTR)"\nVersion: ");
    handle_str_cat(out, outlen, scan->version);
    handle_str_cat(out, outlen, (STRPTR)"\nArch: m68k\n");
    if (scan->os_min[0]) {
        handle_str_cat(out, outlen, (STRPTR)"OS: ");
        handle_str_cat(out, outlen, scan->os_min);
        handle_str_cat(out, outlen, (STRPTR)"\n");
    }
    if (scan->sys_mode) {
        handle_str_cat(out, outlen, (STRPTR)"InstallRoot: Work:\n");
    } else {
        handle_str_cat(out, outlen, (STRPTR)"InstallRoot: Work:");
        handle_str_cat(out, outlen, scan->name);
        handle_str_cat(out, outlen, (STRPTR)"/\n");
    }
    handle_str_cat(out, outlen, (STRPTR)"Summary: ");
    handle_str_cat(out, outlen, scan->summary);
    handle_str_cat(out, outlen, (STRPTR)"\n");
    for (i = 0; i < scan->nreqs; i++) {
        handle_str_cat(out, outlen, (STRPTR)"Requires: ");
        handle_str_cat(out, outlen, scan->reqs[i].name);
        if (scan->reqs[i].minver[0]) {
            handle_str_cat(out, outlen, (STRPTR)" Min=");
            handle_str_cat(out, outlen, scan->reqs[i].minver);
        }
        if (scan->reqs[i].url[0]) {
            handle_str_cat(out, outlen, (STRPTR)" URL=");
            handle_str_cat(out, outlen, scan->reqs[i].url);
        }
        handle_str_cat(out, outlen, (STRPTR)"\n");
    }
    handle_str_cat(out, outlen, (STRPTR)"\n");
    for (i = 0; i < scan->nfiles; i++) {
        handle_str_cat(out, outlen, (STRPTR)"File: ");
        handle_str_cat(out, outlen, scan->files[i].dest);
        handle_str_cat(out, outlen, (STRPTR)" From=");
        handle_str_cat(out, outlen, scan->files[i].from);
        handle_str_cat(out, outlen, (STRPTR)" CPU=");
        handle_str_cat(out, outlen, scan->files[i].cpu);
        if (scan->files[i].fpu882) {
            handle_str_cat(out, outlen, (STRPTR)" FPU=882");
        } else if (scan->files[i].fpu881) {
            handle_str_cat(out, outlen, (STRPTR)" FPU=881");
        }
        if (scan->files[i].mmu) {
            handle_str_cat(out, outlen, (STRPTR)" MMU=1");
        }
        handle_str_cat(out, outlen, (STRPTR)"\n");
    }
    return 1;
}
