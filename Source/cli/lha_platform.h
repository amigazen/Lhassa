/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_platform.h - LhA CLI build target.
 *
 * Portable ANSI C front-end (stdio/stdlib).  Amiga-only file metadata
 * restore lives in lha_amiga.c behind LHA_AMIGA.
 */

#ifndef LHA_PLATFORM_H
#define LHA_PLATFORM_H

#if defined(HOST)
#define LHA_HOST 1
#endif

#if defined(__AMIGADATE__) || defined(__AMIGA) || defined(__SASC) || defined(AMIGA)
#define LHA_AMIGA 1
#endif

#endif /* LHA_PLATFORM_H */
