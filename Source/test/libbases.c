/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * libbases.c - LhBase and SysBase for SAS/C #pragma libcall dispatch.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>

struct Library *LhBase;
extern struct ExecBase *SysBase;
