/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * malloc.c - pooled malloc/free for liblh inside lh.library
 */

#define __USE_SYSBASE

#include "lh_native_guard.h"

#include <exec/types.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include <stddef.h>
#include <string.h>

struct SignalSemaphore LhPoolSemaphore;
static APTR LhPool;

int lh_malloc_init(void)
{
    InitSemaphore(&LhPoolSemaphore);
    LhPool = CreatePool(MEMF_ANY, 4096, 1024);
    if (LhPool == NULL) {
        return 0;
    }
    return 1;
}

void lh_malloc_exit(void)
{
    if (LhPool != NULL) {
        DeletePool(LhPool);
        LhPool = NULL;
    }
}

void *malloc(size_t size)
{
    size_t *ptr;

    if (LhPool == NULL) {
        return NULL;
    }
    if (size > ((size_t)-1) - sizeof(size_t)) {
        return NULL;
    }
    ObtainSemaphore(&LhPoolSemaphore);
    ptr = (size_t *)AllocPooled(LhPool, sizeof(size_t) + size);
    ReleaseSemaphore(&LhPoolSemaphore);
    if (ptr != NULL) {
        *ptr = size;
        ptr++;
    }
    return (void *)ptr;
}

void free(void *ptr)
{
    size_t *block;
    size_t alloc_size;

    if (ptr != NULL && LhPool != NULL) {
        block = (size_t *)ptr - 1;
        alloc_size = *block + sizeof(size_t);
        ObtainSemaphore(&LhPoolSemaphore);
        FreePooled(LhPool, block, alloc_size);
        ReleaseSemaphore(&LhPoolSemaphore);
    }
}

void *calloc(size_t num, size_t es)
{
    size_t size;
    void *ptr;

    size = num * es;
    if (num != 0 && es != 0 && (size / es) != num) {
        return NULL;
    }
    ptr = malloc(size);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    size_t oldsize;
    void *nptr;

    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    oldsize = *((size_t *)ptr - 1);
    nptr = malloc(size);
    if (nptr == NULL) {
        return NULL;
    }
    memcpy(nptr, ptr, oldsize < size ? oldsize : size);
    free(ptr);
    return nptr;
}
