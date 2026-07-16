/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_variant.c - Secondary-extension tags for stage filenames.
 *
 * Allowed tags (only): .020 .030 .040 .060 .881 .882 .mmu .wos .pup
 * No tag = universal 68000, no FPU/MMU.  FPU/MMU may combine with CPU tags.
 */

#include <exec/types.h>
#include <proto/utility.h>

#include "handle_internal.h"

extern struct Library *UtilityBase;

void handle_variant_clear(struct HandleVariant *v)
{
    if (!v) {
        return;
    }
    v->cpu_rank = 0;
    v->fpu881 = 0;
    v->fpu882 = 0;
    v->mmu = 0;
    v->wos = 0;
    v->pup = 0;
}

static int handle_tag_apply(STRPTR tag, struct HandleVariant *v)
{
    if (Stricmp(tag, (STRPTR)"020") == 0) {
        if (v->cpu_rank < 20) {
            v->cpu_rank = 20;
        }
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"030") == 0) {
        if (v->cpu_rank < 30) {
            v->cpu_rank = 30;
        }
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"040") == 0) {
        if (v->cpu_rank < 40) {
            v->cpu_rank = 40;
        }
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"060") == 0) {
        if (v->cpu_rank < 60) {
            v->cpu_rank = 60;
        }
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"881") == 0) {
        v->fpu881 = 1;
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"882") == 0) {
        v->fpu882 = 1;
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"mmu") == 0) {
        v->mmu = 1;
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"wos") == 0) {
        v->wos = 1;
        return 1;
    }
    if (Stricmp(tag, (STRPTR)"pup") == 0) {
        v->pup = 1;
        return 1;
    }
    return 0;
}

/*
 * Closed set only.  A 3-char alphanumeric suffix that is not in the set fails
 * Handle (unknown secondary extension).  Longer suffixes (.library, .info) stay.
 */
static int handle_tag_is_closed_form(STRPTR tag)
{
    LONG n;
    LONG i;
    UBYTE c;

    if (!tag) {
        return 0;
    }
    n = 0;
    while (tag[n] != '\0') {
        n++;
    }
    if (n != 3) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        c = (UBYTE)tag[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z'))) {
            return 0;
        }
    }
    return 1;
}

/*
 * Strip known tags from the end of leaf.  Stops at .library / other long
 * suffixes.  Unknown 3-char tags fail (return 0).
 */
LONG handle_variant_parse_leaf(STRPTR leaf, STRPTR base_out, LONG baselen,
    struct HandleVariant *v)
{
    char work[HANDLE_VAL_LEN];
    STRPTR dot;
    STRPTR last;
    LONG n;

    handle_variant_clear(v);
    if (!leaf || !base_out || baselen <= 0) {
        return 0;
    }
    Strncpy((STRPTR)work, leaf, HANDLE_VAL_LEN);

    for (;;) {
        last = NULL;
        for (dot = (STRPTR)work; *dot != '\0'; dot++) {
            if (*dot == '.') {
                last = dot;
            }
        }
        if (!last || last == (STRPTR)work) {
            break;
        }
        if (handle_tag_apply(last + 1, v)) {
            *last = '\0';
            continue;
        }
        if (handle_tag_is_closed_form(last + 1)) {
            return 0;
        }
        break;
    }

    n = 0;
    while (work[n] != '\0' && n < baselen - 1) {
        base_out[n] = work[n];
        n++;
    }
    base_out[n] = '\0';
    if (n == 0) {
        return 0;
    }
    return 1;
}

void handle_variant_cpu_string(const struct HandleVariant *v, STRPTR out,
    LONG outlen)
{
    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (!v) {
        Strncpy(out, (STRPTR)"68000", outlen);
        return;
    }
    if (v->wos) {
        Strncpy(out, (STRPTR)"wos", outlen);
        return;
    }
    if (v->pup) {
        Strncpy(out, (STRPTR)"pup", outlen);
        return;
    }
    if (v->cpu_rank >= 60) {
        Strncpy(out, (STRPTR)"68060", outlen);
    } else if (v->cpu_rank >= 40) {
        Strncpy(out, (STRPTR)"68040", outlen);
    } else if (v->cpu_rank >= 30) {
        Strncpy(out, (STRPTR)"68030", outlen);
    } else if (v->cpu_rank >= 20) {
        Strncpy(out, (STRPTR)"68020", outlen);
    } else {
        Strncpy(out, (STRPTR)"68000", outlen);
    }
}
