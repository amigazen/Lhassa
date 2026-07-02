/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright 2026 amigazen project
 *
 * VBCC 68000 runtime helpers __lmodu / __lmods for 32-bit modulo when the
 * CPU has no div.l (68000/68010).  Implemented with utility.library
 * UDivMod32 / SDivMod32 (V36): quotient in D0, remainder in D1.
 *
 * C prototypes only return D0, so remainder is reconstructed as
 * dividend - quotient * divisor (see clib/utility_protos.h).
 *
 * vbcc references __lmodu at link time; define _lmodu in C (one leading _).
 *
 * https://developer.amigaos3.net/autodocs/utility.library/UDivMod32.html
 */

#include <exec/types.h>
#include <stdlib.h>

#include <proto/exec.h>

struct Library *UtilityBase = NULL;

#include <proto/utility.h>

static int lh_utility_atexit_registered = 0;

static void lh_close_utility(void)
{
    if (UtilityBase != NULL) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
}

void lh_utility_shutdown(void)
{
    lh_close_utility();
}

static int lh_ensure_utility(void)
{
    if (UtilityBase != NULL) {
        return 1;
    }
    UtilityBase = OpenLibrary("utility.library", 36L);
    if (UtilityBase == NULL) {
        return 0;
    }
    if (!lh_utility_atexit_registered) {
        atexit(lh_close_utility);
        lh_utility_atexit_registered = 1;
    }
    return 1;
}

unsigned long
_lmodu(unsigned long dividend, unsigned long divisor)
{
    ULONG quotient;

    if (!lh_ensure_utility()) {
        return 0UL;
    }
    if (divisor == 0UL) {
        return 0UL;
    }
    quotient = UDivMod32((ULONG)dividend, (ULONG)divisor);
    return (ULONG)dividend - UMult32(quotient, (ULONG)divisor);
}

long
_lmods(long dividend, long divisor)
{
    LONG quotient;
    long result;

    if (!lh_ensure_utility()) {
        return 0L;
    }
    if (divisor == 0L) {
        return 0L;
    }
    quotient = SDivMod32(dividend, divisor);
    result = dividend - SMult32(quotient, divisor);
    return result;
}
