/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * proto/lh.h - lh.library client include (prototypes + pragmas/inlines)
 * Based on LibDescConverter output from SDK/SFD/lh_lib.sfd.
 */

#ifndef PROTO_LH_H
#define PROTO_LH_H

#ifdef _NO_INLINE

#include <clib/lh_protos.h>

#else

#ifndef __NOLIBBASE__

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif

extern struct Library *LhBase;
#endif /* __NOLIBBASE__ */

#if defined(LATTICE) || defined(__SASC) || defined(__SASC__) || defined(_DCC)

#ifndef PRAGMAS_LH_PRAGMAS_H
#include <pragmas/lh_pragmas.h>
#endif

#elif defined(AZTEC_C) || defined(__MAXON__) || defined(__STORM__)

#ifndef PRAGMA_LH_LIB_H
#include <pragma/lh_lib.h>
#endif

#elif defined(__VBCC__)

#include <clib/lh_protos.h>

#ifndef INLINE_LH_PROTOS_H
#include <inline/lh_protos.h>
#endif

#elif defined(__GNUC__)

#if defined(mc68000)
#ifndef INLINE_LH_H
#include <inline/lh.h>
#endif
#else
#include <clib/lh_protos.h>
#endif

#else

#include <clib/lh_protos.h>

#endif /* compiler */

#endif /* _NO_INLINE */

#endif /* PROTO_LH_H */
