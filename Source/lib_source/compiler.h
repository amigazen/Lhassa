/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * compiler.h - cross-compiler attributes for lh.library
 */

#ifndef LH_COMPILER_H
#define LH_COMPILER_H

#include <exec/types.h>
#include <clib/compiler-specific.h>
#include <proto/exec.h>

#ifndef LH_INITTABLE_DEFINED
#define LH_INITTABLE_DEFINED 1
struct InitTable
{
    ULONG it_LibSize;
    APTR *it_FuncTable;
    APTR  it_DataTable;
    APTR  it_InitFunc;
};
#endif

struct MyDataInit
{
    ULONG md_Init[19];
};

#endif /* LH_COMPILER_H */
