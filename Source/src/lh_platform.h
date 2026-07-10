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

/*
 * Amiga SAS/C (DEF=AMIGA / __SASC) always wins.  Preferring HOST first let a
 * leaked HOST define strip DateStamp helpers from the library core.
 */
#if defined(__AMIGA) || defined(__SASC) || defined(__AMIGADATE__) || defined(AMIGA)
#if defined(HOST) || defined(LH_HOST)
#error "lh core: HOST is incompatible with Amiga SAS/C build (use DEF=AMIGA only)"
#endif
#define LH_AMIGA 1
#elif defined(HOST)
#define LH_HOST 1
#endif

#endif /* LH_PLATFORM_H */
