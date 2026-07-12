/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Default Workbench .info payloads for magic placeholders (Disk.info,
 * drawer/.lha.info when no real host .info exists).
 *
 * Filled at startup from GetDefDiskObject via icon.library PutDiskObject
 * into AllocMem (lh_icon.c).  Drawer has an embedded fallback only.
 */

#ifndef LH_DRAWER_INFO_H
#define LH_DRAWER_INFO_H

#include <exec/types.h>

#define LHH_VINFO_DRAWER   0
#define LHH_VINFO_DISK     1
#define LHH_VINFO_TOOL     2
#define LHH_VINFO_PROJECT  3

int lhh_icon_init(APTR gd);
void lhh_icon_cleanup(APTR gd);

const UBYTE *lhh_icon_data(int kind);
ULONG lhh_icon_len(int kind);

extern const UBYTE lhh_drawer_info_fallback[];
extern const ULONG lhh_drawer_info_fallback_len;

#define lhh_drawer_info_data       (lhh_icon_data(LHH_VINFO_DRAWER))
#define lhh_drawer_info_data_len   (lhh_icon_len(LHH_VINFO_DRAWER))
#define lhh_disk_info_data         (lhh_icon_data(LHH_VINFO_DISK))
#define lhh_disk_info_data_len     (lhh_icon_len(LHH_VINFO_DISK))

#endif /* LH_DRAWER_INFO_H */
