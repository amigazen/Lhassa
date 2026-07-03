/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_native_guard.h - lh.library shell must be Amiga-native (no HOST stdio build).
 */

#ifndef LH_NATIVE_GUARD_H
#define LH_NATIVE_GUARD_H

#include "/src/lh_platform.h"

#if defined(LH_HOST) || defined(HOST)
#error "lh.library shell: do not build with HOST=1; use SAS/C DEF=AMIGA"
#endif

#ifndef LH_AMIGA
#error "lh.library shell requires LH_AMIGA (__SASC, DEF=AMIGA, or __AMIGADATE__)"
#endif

#endif /* LH_NATIVE_GUARD_H */
