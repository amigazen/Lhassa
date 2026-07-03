/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_platform.h - build target for shared library core (Source/src).
 *
 *   lh.library (SAS/C, DEF=AMIGA)  -> LH_AMIGA: dos.library I/O, pooled malloc.
 *   liblh.a smoke-test (make HOST=1) -> LH_HOST: ANSI stdio/stdlib only.
 *
 * See LIBC_TO_AMIGA.md for libc vs native mapping on Amiga builds.
 */

#ifndef LH_PLATFORM_H
#define LH_PLATFORM_H

#if defined(LH_AMIGA) && (defined(LH_HOST) || defined(HOST))
#error "lh core: LH_AMIGA and LH_HOST/HOST are mutually exclusive"
#endif

#if defined(HOST)
#define LH_HOST 1
#elif defined(__AMIGA) || defined(__SASC) || defined(__AMIGADATE__) || defined(AMIGA)
#define LH_AMIGA 1
#endif

#endif /* LH_PLATFORM_H */
