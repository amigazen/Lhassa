/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * pragmas/lh_pragmas.h - SAS/C #pragma libcall for lh.library
 * Generated from SDK/SFD/lh_lib.sfd (LibDescConverter); hand-edited:
 * Offsets MUST match FuncTab[] in Source/lib_source/StartUp.c.
 */

#ifndef PRAGMAS_LH_PRAGMAS_H
#define PRAGMAS_LH_PRAGMAS_H

#ifndef CLIB_LH_PROTOS_H
#include <clib/lh_protos.h>
#endif

#if defined(__SASC) || defined(__SASC__) || defined(_DCC)
#pragma libcall LhBase CreateBuffer 1e 1
#pragma libcall LhBase DeleteBuffer 24 801
#pragma libcall LhBase LhEncode 2a 801
#pragma libcall LhBase LhDecode 30 801
#pragma libcall LhBase LhCompress 36 8002
#pragma libcall LhBase LhDecompress 3c 8002
#pragma libcall LhBase LhOpenArchive 42 0102
#pragma libcall LhBase LhCloseArchive 48 801
#pragma libcall LhBase LhLock 4e 9802
#pragma libcall LhBase LhUnLock 54 801
#pragma libcall LhBase LhExamine 5a 9802
#pragma libcall LhBase LhExNext 60 9802
#pragma libcall LhBase LhExAll 66 9201805
#pragma libcall LhBase LhExAllEnd 6c 801
#pragma libcall LhBase LhInfo 72 9802
#pragma libcall LhBase LhOpenFromLock 78 0802
#pragma libcall LhBase LhOpen 7e 09803
#pragma libcall LhBase LhRead 84 08103
#pragma libcall LhBase LhWrite 8a 08103
#pragma libcall LhBase LhClose 90 101
#pragma libcall LhBase LhSeek 96 02103
#pragma libcall LhBase LhNameFromLock 9c 09803
#pragma libcall LhBase LhAddEntry a2 0A9804
#pragma libcall LhBase LhDeleteFile a8 9802
#pragma libcall LhBase LhConcatArchive ae 9802
#pragma libcall LhBase LhSetPassword b4 9802
#pragma libcall LhBase LhReadData ba A9803
#pragma libcall LhBase LhExtractEntry c0 A9803
#pragma libcall LhBase LhTestEntry c6 9802
#pragma libcall LhBase LhPrintEntry cc 9802
#pragma libcall LhBase LhErr d2 00
#endif

#endif /* PRAGMAS_LH_PRAGMAS_H */
