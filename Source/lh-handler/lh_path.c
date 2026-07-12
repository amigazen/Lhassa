/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_path.c - Virtual LHA: path helpers and archive/entry split.
 */

#include "lh-handler.h"

void lhh_bstr_to_cstr(STRPTR bstr, STRPTR out, LONG outlen)
{
    LONG n;
    LONG i;

    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (!bstr) {
        return;
    }
    n = (LONG)((UBYTE *)bstr)[0];
    if (n >= outlen) {
        n = outlen - 1;
    }
    for (i = 0; i < n; i++) {
        out[i] = ((char *)bstr)[i + 1];
    }
    out[n] = '\0';
}

void lhh_cstr_copy(STRPTR dst, const char *src, LONG dstlen)
{
    LONG i;

    if (!dst || dstlen <= 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i < dstlen - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

LONG lhh_cstr_len(const char *s)
{
    LONG n;

    if (!s) {
        return 0;
    }
    n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int lhh_cstr_eq(const char *a, const char *b)
{
    LONG i;

    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    for (i = 0; a[i] || b[i]; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int lhh_ch_eq_i(char a, char b)
{
    if (a >= 'A' && a <= 'Z') {
        a = (char)(a + ('a' - 'A'));
    }
    if (b >= 'A' && b <= 'Z') {
        b = (char)(b + ('a' - 'A'));
    }
    return a == b;
}

int lhh_cstr_eq_i(const char *a, const char *b)
{
    LONG i;

    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    for (i = 0; a[i] || b[i]; i++) {
        if (!lhh_ch_eq_i(a[i], b[i])) {
            return 0;
        }
    }
    return 1;
}

void lhh_path_join(STRPTR out, LONG outlen, const char *base, const char *name)
{
    LONG bl;
    LONG i;
    LONG j;

    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (!name || !name[0]) {
        if (base) {
            lhh_cstr_copy(out, base, outlen);
        }
        return;
    }
    if (!base || !base[0]) {
        lhh_cstr_copy(out, name, outlen);
        return;
    }
    bl = lhh_cstr_len(base);
    for (i = 0; i < bl && i < outlen - 1; i++) {
        out[i] = base[i];
    }
    if (i > 0 && out[i - 1] != '/' && out[i - 1] != ':' && i < outlen - 1) {
        out[i++] = '/';
    }
    for (j = 0; name[j] && i < outlen - 1; j++, i++) {
        out[i] = name[j];
    }
    out[i] = '\0';
}

/*
 * Amiga locate packets carry one name component; "/" alone means parent.
 * Full path strings still use ':' for volume root and '/' between levels
 * (RAM:foo/bar), but never pass multi-component names in one host locate.
 */
int lhh_is_parent_name(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    return (name[0] == '/' && name[1] == '\0');
}

/*
 * True when name is one or more '/' only ("/", "//", ...).  Amiga cd uses
 * these as parent steps; they must not be stripped or treated as entries.
 */
int lhh_is_parent_chain(const char *name)
{
    LONG i;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] != '/') {
            return 0;
        }
    }
    return 1;
}

/*
 * True for names ending in ".lha.info" (any case), e.g. Foo.lha.info.
 * Used to serve the embedded Drawer.info for archive icons.
 */
int lhh_is_lha_info_name(const char *name)
{
    static const char suf[] = ".lha.info";
    LONG n;
    LONG i;
    char a;
    char b;

    if (name == NULL) {
        return 0;
    }
    n = lhh_cstr_len(name);
    if (n < 9) {
        return 0;
    }
    for (i = 0; i < 9; i++) {
        a = name[n - 9 + i];
        b = suf[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* name ends with ".info" (any case) -> base without suffix. */
int lhh_strip_info_suffix(const char *name, STRPTR base, LONG baselen)
{
    LONG n;
    LONG i;
    char a;
    char b;
    static const char suf[] = ".info";

    if (!name || !base || baselen <= 0) {
        return 0;
    }
    n = lhh_cstr_len(name);
    if (n < 5) {
        return 0;
    }
    for (i = 0; i < 5; i++) {
        a = name[n - 5 + i];
        b = suf[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    if (n - 5 >= baselen) {
        return 0;
    }
    for (i = 0; i < n - 5; i++) {
        base[i] = name[i];
    }
    base[n - 5] = '\0';
    return base[0] ? 1 : 0;
}

/* True if name is ".info" or ends with ".info" (any case). */
int lhh_name_ends_info(const char *name)
{
    LONG n;
    LONG i;
    char a;
    char b;
    static const char suf[] = ".info";

    if (name == NULL) {
        return 0;
    }
    if (lhh_cstr_eq_i(name, ".info")) {
        return 1;
    }
    n = lhh_cstr_len(name);
    if (n < 5) {
        return 0;
    }
    for (i = 0; i < 5; i++) {
        a = name[n - 5 + i];
        b = suf[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* Leaf or path ends with ".lha" (any case). */
int lhh_name_ends_lha(const char *name)
{
    LONG n;
    LONG i;
    char a;
    char b;
    static const char suf[] = ".lha";

    if (name == NULL) {
        return 0;
    }
    n = lhh_cstr_len(name);
    if (n < 4) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        a = name[n - 4 + i];
        b = suf[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/*
 * True if a multi-component path goes through an archive file before the
 * last component (e.g. Work/t/foo.lha/bar.info).  Virtual .info placeholders
 * are only for objects outside archives.
 */
int lhh_path_crosses_lha(const char *path)
{
    const char *p;
    char comp[LHH_PATH_LEN];
    int saw_lha;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (!lhh_cstr_has(path, '/')) {
        return 0;
    }
    saw_lha = 0;
    p = path;
    while (lhh_path_next(&p, comp, LHH_PATH_LEN)) {
        if (p[0] == '\0') {
            /* Last component - ignore whether it is .lha */
            break;
        }
        if (lhh_name_ends_lha(comp)) {
            saw_lha = 1;
            break;
        }
    }
    return saw_lha;
}

/* Disk.info / Volume.info / bare "Disk" for LHA: volume icon. */
int lhh_is_disk_info_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (lhh_cstr_eq_i(name, "Disk")) {
        return 1;
    }
    if (lhh_cstr_eq_i(name, "Disk.info")) {
        return 1;
    }
    if (lhh_cstr_eq_i(name, "Volume.info")) {
        return 1;
    }
    return 0;
}

/*
 * Take the next LHA: path component from *path (or "/" for parent).
 * Returns 0 when nothing left.
 */
int lhh_path_next(const char **path, char *comp, LONG complen)
{
    const char *p;
    LONG i;

    if (path == NULL || comp == NULL || complen <= 0) {
        return 0;
    }
    p = *path;
    if (p == NULL || p[0] == '\0') {
        return 0;
    }
    if (p[0] == '/' && (p[1] == '\0' || p[1] == '/')) {
        comp[0] = '/';
        comp[1] = '\0';
        p++;
        while (*p == '/') {
            p++;
        }
        *path = p;
        return 1;
    }
    if (p[0] == '/') {
        p++;
    }
    i = 0;
    while (p[i] != '\0' && p[i] != '/' && i < complen - 1) {
        comp[i] = p[i];
        i++;
    }
    comp[i] = '\0';
    p += i;
    if (*p == '/') {
        p++;
    }
    *path = p;
    return comp[0] ? 1 : 0;
}

/*
 * Append one path component to a full Amiga path string.
 * RAM: + foo -> RAM:foo ; RAM:foo + bar -> RAM:foo/bar
 */
void lhh_real_append_name(STRPTR out, LONG outlen, const char *base,
    const char *name)
{
    LONG i;
    LONG j;
    int ends_colon;

    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (!name || !name[0]) {
        if (base) {
            lhh_cstr_copy(out, base, outlen);
        }
        return;
    }
    if (!base || !base[0]) {
        lhh_cstr_copy(out, name, outlen);
        return;
    }
    ends_colon = 0;
    for (i = 0; base[i] != '\0' && i < outlen - 1; i++) {
        out[i] = base[i];
        if (base[i] == ':' && base[i + 1] == '\0') {
            ends_colon = 1;
        }
    }
    if (i > 0 && out[i - 1] == ':') {
        ends_colon = 1;
    }
    if (!ends_colon && i > 0 && out[i - 1] != '/' && i < outlen - 1) {
        out[i++] = '/';
    }
    for (j = 0; name[j] != '\0' && i < outlen - 1; j++, i++) {
        out[i] = name[j];
    }
    out[i] = '\0';
}

/* Join real Amiga paths (single component in name). */
void lhh_real_join(STRPTR out, LONG outlen, const char *base, const char *name)
{
    lhh_real_append_name(out, outlen, base, name);
}

/*
 * Append one component to a virtual path under LHA: (no LHA: prefix).
 * "" + VolumeName -> VolumeName ; VolumeName + lhasa -> VolumeName/lhasa
 * Virtual paths never contain ':'.
 */
void lhh_virt_append_name(STRPTR out, LONG outlen, const char *base,
    const char *name)
{
    LONG i;
    LONG j;

    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (!name || !name[0]) {
        if (base) {
            lhh_cstr_copy(out, base, outlen);
        }
        return;
    }
    if (!base || !base[0]) {
        /* First component under LHA: root is a bare volume/assign name. */
        lhh_cstr_copy(out, name, outlen);
        return;
    }
    for (i = 0; base[i] != '\0' && i < outlen - 1; i++) {
        out[i] = base[i];
    }
    if (i > 0 && out[i - 1] != '/' && i < outlen - 1) {
        out[i++] = '/';
    }
    for (j = 0; name[j] != '\0' && i < outlen - 1; j++, i++) {
        out[i] = name[j];
    }
    out[i] = '\0';
}

/* Parent of a virtual path under LHA: (slash-separated, no colons). */
int lhh_virt_parent_path(const char *virt, STRPTR out, LONG outlen)
{
    if (!virt || !virt[0]) {
        return 0;
    }
    /* Bare volume name under LHA: has no parent under LHA:. */
    if (!lhh_cstr_has(virt, '/')) {
        return 0;
    }
    return lhh_parent_path(virt, out, outlen);
}

/*
 * Map a real Amiga path to the virtual form under LHA: (no prefix).
 * VolumeName: -> VolumeName ; VolumeName:lhasa/file.lha -> VolumeName/lhasa/file.lha
 */
int lhh_real_to_virt_path(GD gd, const char *real, STRPTR virt, LONG outlen)
{
    static char vol[108];
    static char canon[108];
    LONG i;
    LONG j;
    LONG vlen;

    if (!real || !real[0] || !virt || outlen <= 0) {
        return 0;
    }
    virt[0] = '\0';
    vlen = 0;
    for (i = 0; real[i] != '\0' && real[i] != ':'; i++) {
        if (vlen < (LONG)sizeof(vol) - 1) {
            vol[vlen++] = real[i];
        }
    }
    vol[vlen] = '\0';
    if (vol[0] == '\0') {
        return 0;
    }
    if (!lhh_canon_volume(gd, vol, canon, (LONG)sizeof(canon))) {
        lhh_cstr_copy(canon, vol, (LONG)sizeof(canon));
    }
    j = 0;
    for (i = 0; canon[i] != '\0' && j < outlen - 1; i++, j++) {
        virt[j] = canon[i];
    }
    if (real[i] == ':') {
        i++;
    }
    if (real[i] != '\0') {
        if (j < outlen - 1) {
            virt[j++] = '/';
        }
        for (; real[i] != '\0' && j < outlen - 1; i++, j++) {
            virt[j] = real[i];
        }
    }
    virt[j] = '\0';
    return 1;
}

/*
 * Fill fib_FileName (BSTR) from the last component of a virtual path.
 * NameFromLock() / MatchFirst(ap_Strlen) walk Parent+Examine on handler locks.
 */
void lhh_examine_fib_name(struct FileInfoBlock *fib, const char *virt)
{
    static char comp[108];
    LONG n;
    LONG i;

    if (fib == NULL) {
        return;
    }
    if (!virt || !virt[0]) {
        fib->fib_FileName[0] = 0;
        return;
    }
    lhh_file_part(virt, comp, (LONG)sizeof(comp));
    n = lhh_cstr_len(comp);
    if (n > 106) {
        n = 106;
    }
    fib->fib_FileName[0] = (UBYTE)n;
    for (i = 0; i < n; i++) {
        fib->fib_FileName[i + 1] = comp[i];
    }
    fib->fib_FileName[n + 1] = '\0';
}

/*
 * Convert one NUL-terminated field in a FIB to a BSTR (length at [0]).
 * Used when a dos-style C string from Lh* must go out on a handler packet.
 */
static void lhh_field_cstr_to_bstr(UBYTE *field, LONG fieldmax)
{
    static char tmp[108];
    LONG n;
    LONG i;

    if (field == NULL || fieldmax < 2) {
        return;
    }
    n = 0;
    while (field[n] != '\0' && n < fieldmax - 2 && n < (LONG)sizeof(tmp) - 1) {
        tmp[n] = (char)field[n];
        n++;
    }
    if (n > 255) {
        n = 255;
    }
    field[0] = (UBYTE)n;
    for (i = 0; i < n; i++) {
        field[i + 1] = (UBYTE)tmp[i];
    }
    field[n + 1] = '\0';
}

void lhh_fib_cstr_to_bstr(struct FileInfoBlock *fib)
{
    if (fib == NULL) {
        return;
    }
    lhh_field_cstr_to_bstr((UBYTE *)fib->fib_FileName,
        (LONG)sizeof(fib->fib_FileName));
    lhh_field_cstr_to_bstr((UBYTE *)fib->fib_Comment,
        (LONG)sizeof(fib->fib_Comment));
}

int lhh_cstr_has(const char *s, char ch)
{
    LONG i;

    if (!s) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        if (s[i] == ch) {
            return 1;
        }
    }
    return 0;
}

/*
 * RKRM: handlers must not call LockDosList (can deadlock).  AttemptLockDosList
 * returns a DosList pointer on success, or 0/1 on failure (V39 quirk).
 * Spin briefly; we cannot Delay/WaitPort while holding a client packet.
 */
struct DosList *lhh_attempt_lock_doslist(GD gd, ULONG flags)
{
    struct DosList *dl;
    LONG i;

    for (i = 0; i < 128; i++) {
        dl = AttemptLockDosList(flags);
        if (dl != NULL && (ULONG)dl > 1UL) {
            return dl;
        }
    }
    return NULL;
}

/*
 * Look up vol (no trailing ':') in the dos list case-insensitively and
 * copy the canonical name to out.  Covers devices, volumes, and assigns.
 * If the DosList cannot be locked, copy vol as-is so DeviceProc can still
 * resolve (volume names are case-insensitive).
 */
int lhh_canon_volume(GD gd, const char *vol, STRPTR out, LONG outlen)
{
    struct DosList *dl;
    struct DosList *e;
    STRPTR bname;
    LONG n;
    LONG i;
    LONG vlen;
    ULONG flags;

    if (!vol || !vol[0] || !out || outlen <= 1) {
        return 0;
    }
    vlen = lhh_cstr_len(vol);
    /* Ignore a trailing colon if the caller passed "RAM:" */
    if (vlen > 0 && vol[vlen - 1] == ':') {
        vlen--;
    }
    if (vlen <= 0) {
        return 0;
    }

    flags = LDF_DEVICES | LDF_VOLUMES | LDF_ASSIGNS | LDF_READ;
    dl = lhh_attempt_lock_doslist(gd, flags);
    if (dl == NULL) {
        /* DosList busy - pass name through unchanged. */
        if (vlen >= outlen) {
            vlen = outlen - 1;
        }
        for (i = 0; i < vlen; i++) {
            out[i] = vol[i];
        }
        out[vlen] = '\0';
        return 1;
    }
    e = dl;
    while ((e = NextDosEntry(e, flags)) != NULL) {
        bname = (STRPTR)BADDR(e->dol_Name);
        if (bname == NULL) {
            continue;
        }
        n = (LONG)((UBYTE *)bname)[0];
        if (n != vlen) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (!lhh_ch_eq_i(bname[i + 1], vol[i])) {
                break;
            }
        }
        if (i == n) {
            if (n >= outlen) {
                n = outlen - 1;
            }
            for (i = 0; i < n; i++) {
                out[i] = bname[i + 1];
            }
            out[n] = '\0';
            UnLockDosList(flags);
            return 1;
        }
    }
    UnLockDosList(flags);
    return 0;
}

/*
 * Virtual path under LHA: first component is a volume/assign/device name
 * (case-insensitive).  LHA:ram/foo/bar.lha -> RAM:foo/bar.lha
 *
 * Colons are not allowed in virtual paths.  LHA:VolumeName: is invalid;
 * LHA:VolumeName maps to VolumeName:.
 */
int lhh_virt_to_real(GD gd, const char *virt, STRPTR real, LONG reallen)
{
    static char vol[108];
    static char canon[108];
    const char *rest;
    LONG vi;
    LONG i;
    LONG j;

    if (!virt || !virt[0] || !real || reallen <= 0) {
        return 0;
    }
    while (*virt == '/') {
        virt++;
    }
    if (!virt[0]) {
        return 0;
    }
    /* Virtual LHA: paths use '/' only - never Amiga volume syntax. */
    if (lhh_cstr_has(virt, ':')) {
        return 0;
    }

    /* Split volume from the rest at the first '/'. */
    vi = 0;
    rest = NULL;
    for (i = 0; virt[i]; i++) {
        if (virt[i] == '/') {
            rest = virt + i + 1;
            break;
        }
        if (vi < (LONG)sizeof(vol) - 1) {
            vol[vi++] = virt[i];
        }
    }
    vol[vi] = '\0';
    if (vol[0] == '\0') {
        return 0;
    }

    if (!lhh_canon_volume(gd, vol, canon, (LONG)sizeof(canon))) {
        return 0;
    }

    /* LHA:VolumeName -> VolumeName: */
    j = 0;
    for (i = 0; canon[i] && j < reallen - 1; i++, j++) {
        real[j] = canon[i];
    }
    if (j < reallen - 1) {
        real[j++] = ':';
    }
    if (rest != NULL && rest[0] != '\0') {
        for (i = 0; rest[i] && j < reallen - 1; i++, j++) {
            real[j] = rest[i];
        }
    }
    real[j] = '\0';
    return 1;
}

int lhh_is_self_volume(GD gd, const char *name)
{
    STRPTR bname;
    LONG n;
    LONG i;

    if (!gd || !gd->gd_DosList || !name || !name[0]) {
        return 0;
    }
    bname = (STRPTR)BADDR(gd->gd_DosList->dol_Name);
    if (!bname) {
        return 0;
    }
    n = (LONG)((UBYTE *)bname)[0];
    for (i = 0; i < n; i++) {
        if (name[i] == '\0' || !lhh_ch_eq_i(name[i], bname[i + 1])) {
            return 0;
        }
    }
    return name[n] == '\0' || name[n] == '/' || name[n] == ':';
}

/* True if real path refers to our volume (LHA:...). */
int lhh_is_self_real(GD gd, const char *real)
{
    STRPTR bname;
    LONG n;
    LONG i;

    if (!gd || !gd->gd_DosList || !real || !real[0]) {
        return 0;
    }
    bname = (STRPTR)BADDR(gd->gd_DosList->dol_Name);
    if (!bname) {
        return 0;
    }
    n = (LONG)((UBYTE *)bname)[0];
    for (i = 0; i < n; i++) {
        if (real[i] == '\0' || !lhh_ch_eq_i(real[i], bname[i + 1])) {
            return 0;
        }
    }
    return real[n] == ':' || real[n] == '\0';
}

/*
 * True if name (C string) is the LHA: volume root itself: empty, slashes
 * only, or our device name with optional trailing colon and nothing more.
 * dos/MatchFirst sometimes delivers "LHA" / "LHA:" instead of an empty
 * BSTR; treating those as NOT_FOUND makes list LHA: fail while
 * root-handler succeeds (it ignores the name entirely).
 */
int lhh_is_volume_root_name(GD gd, const char *name)
{
    STRPTR bname;
    LONG n;
    LONG i;
    LONG j;

    if (name == NULL || name[0] == '\0') {
        return 1;
    }
    /*
     * Workbench locates BSTR ":" on the handler
     * port (volume root).  Same meaning as empty name under LHA:.
     */
    if (name[0] == ':' && name[1] == '\0') {
        return 1;
    }
    i = 0;
    while (name[i] == '/') {
        i++;
    }
    if (name[i] == '\0') {
        return 1;
    }
    if (!gd || !gd->gd_DosList) {
        return 0;
    }
    bname = (STRPTR)BADDR(gd->gd_DosList->dol_Name);
    if (!bname) {
        return 0;
    }
    n = (LONG)((UBYTE *)bname)[0];
    for (j = 0; j < n; j++) {
        if (name[i + j] == '\0' || !lhh_ch_eq_i(name[i + j], bname[j + 1])) {
            return 0;
        }
    }
    /* "LHA" or "LHA:" only - not "LHA:foo" or "LHA/foo". */
    return name[i + n] == '\0' ||
        (name[i + n] == ':' && name[i + n + 1] == '\0');
}

/*
 * Shell/MatchFirst sometimes pass "lha:path" (device + colon) as the locate
 * name instead of "path".  Strip our own prefix so nested locks work.
 */
int lhh_strip_self_prefix(GD gd, char *name)
{
    STRPTR bname;
    LONG n;
    LONG i;
    LONG j;

    if (!gd || !gd->gd_DosList || !name || !name[0]) {
        return 0;
    }
    bname = (STRPTR)BADDR(gd->gd_DosList->dol_Name);
    if (!bname) {
        return 0;
    }
    n = (LONG)((UBYTE *)bname)[0];
    for (i = 0; i < n; i++) {
        if (name[i] == '\0' || !lhh_ch_eq_i(name[i], bname[i + 1])) {
            return 0;
        }
    }
    if (name[n] != ':') {
        return 0;
    }
    /* Shift remainder left over "LHA:" */
    j = 0;
    for (i = n + 1; name[i] != '\0'; i++, j++) {
        name[j] = name[i];
    }
    name[j] = '\0';
    return 1;
}

void lhh_file_part(const char *path, STRPTR out, LONG outlen)
{
    LONG i;
    LONG start;

    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (!path) {
        return;
    }
    start = 0;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == ':') {
            start = i + 1;
        }
    }
    lhh_cstr_copy(out, path + start, outlen);
}

int lhh_parent_path(const char *path, STRPTR out, LONG outlen)
{
    LONG i;
    LONG last;

    if (!path || !out || outlen <= 0) {
        return 0;
    }
    last = -1;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/') {
            last = i;
        }
    }
    if (last < 0) {
        /* Volume root: "System:file.lha" -> "System:" */
        for (i = 0; path[i]; i++) {
            if (path[i] == ':') {
                if (i + 1 >= outlen) {
                    return 0;
                }
                lhh_cstr_copy(out, path, i + 2);
                out[i + 1] = '\0';
                return 1;
            }
        }
        return 0;
    }
    if (last >= outlen) {
        return 0;
    }
    for (i = 0; i < last; i++) {
        out[i] = path[i];
    }
    out[last] = '\0';
    /* Ensure volume root ends with ':' when parent is the volume */
    for (i = 0; out[i]; i++) {
        if (out[i] == ':') {
            if (out[i + 1] == '\0') {
                return 1;
            }
            return 1;
        }
    }
    return 1;
}

/*
 * Walk virtual path components; at each cumulative real path try
 * LhOpenArchive.  First success is the archive; remainder is entry name.
 */
int lhh_split_archive(GD gd, const char *virt,
    STRPTR real_out, LONG real_len,
    STRPTR entry_out, LONG entry_len)
{
    static char work[LHH_PATH_LEN];
    static char try_path[LHH_PATH_LEN];
    struct LhArchive *arc;
    LONG i;
    LONG start;
    int found;

    if (!virt || !virt[0] || !real_out || !entry_out) {
        return 0;
    }
    real_out[0] = '\0';
    entry_out[0] = '\0';

    lhh_cstr_copy(work, virt, (LONG)sizeof(work));
    /* Strip leading slashes */
    start = 0;
    while (work[start] == '/') {
        start++;
    }
    if (!work[start]) {
        return 0;
    }

    found = 0;

    /*
     * Try cumulative prefixes ending at each '/' (and the full path).
     * Convert each virtual prefix to a real Amiga path and probe.
     * Prefer the longest prefix that opens as a valid archive.
     */
    for (i = start; work[i]; i++) {
        if (work[i] != '/' && work[i + 1] != '\0') {
            continue;
        }
        if (work[i] == '/') {
            work[i] = '\0';
            if (!lhh_virt_to_real(gd, work + start, try_path,
                    (LONG)sizeof(try_path))) {
                work[i] = '/';
                continue;
            }
            work[i] = '/';
        } else {
            if (!lhh_virt_to_real(gd, work + start, try_path,
                    (LONG)sizeof(try_path))) {
                continue;
            }
        }

        /* Never open our own volume ? LhOpenArchive would Lock LHA: and deadlock. */
        if (lhh_is_self_real(gd, try_path)) {
            if (work[i] != '/') {
                break;
            }
            continue;
        }

        arc = LhOpenArchive((STRPTR)try_path, LHARC_MODE_READ);
        if (arc != NULL) {
            LhCloseArchive(arc);
            found = 1;
            lhh_cstr_copy(real_out, try_path, real_len);
            if (work[i] == '/') {
                lhh_cstr_copy(entry_out, work + i + 1, entry_len);
            } else {
                entry_out[0] = '\0';
            }
            if (work[i] != '/') {
                break;
            }
        }
        if (work[i] != '/') {
            break;
        }
    }

    return found;
}
