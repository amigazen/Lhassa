/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_archive_dos.h - Internal dos.library-style archive engine for lh.library
 */

#ifndef LH_ARCHIVE_DOS_H
#define LH_ARCHIVE_DOS_H

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/exall.h>
#include <utility/tagitem.h>

#define LHARC_MODE_READ   MODE_OLDFILE
#define LHARC_MODE_WRITE  MODE_NEWFILE
#define LHARC_MODE_APPEND 1007L

/* LhAddEntryTagList / LhAddEntryTags tags (must match libraries/lhlib.h). */
#ifndef LHADD_Method
#define LHADD_Method     (TAG_USER + 1)
#define LHADD_Attrs      (TAG_USER + 2)
#define LHADD_DateStamp  (TAG_USER + 3)
#define LHADD_Comment    (TAG_USER + 4)
#define LHADD_Directory  (TAG_USER + 5)
#endif

struct LhArchive;
struct LhLock;
struct LhFileHandle;

struct LhArchive *lh_arc_open(STRPTR path, LONG mode);
LONG lh_arc_close(struct LhArchive *archive);

BPTR lh_arc_lock(struct LhArchive *archive, STRPTR name);
LONG lh_arc_unlock(BPTR lock);

LONG lh_arc_examine(BPTR lock, struct FileInfoBlock *fib);
LONG lh_arc_exnext(BPTR lock, struct FileInfoBlock *fib);
LONG lh_arc_exall(BPTR lock, STRPTR buffer, LONG size, LONG type,
    struct ExAllControl *control);
LONG lh_arc_exall_end(BPTR lock);
LONG lh_arc_info(BPTR lock, struct InfoData *info);

BPTR lh_arc_open_from_lock(BPTR lock, LONG mode);
BPTR lh_arc_open_entry(struct LhArchive *archive, STRPTR name, LONG mode);
LONG lh_arc_read_data(struct LhArchive *archive, STRPTR name, APTR *data_out);
LONG lh_arc_extract_entry(struct LhArchive *archive, STRPTR name, STRPTR dest);
LONG lh_arc_test_entry(struct LhArchive *archive, STRPTR name);
LONG lh_arc_print_entry(struct LhArchive *archive, STRPTR name);
LONG lh_arc_read(BPTR fh, APTR buffer, LONG len);
LONG lh_arc_write(BPTR fh, APTR buffer, LONG len);
LONG lh_arc_closefh(BPTR fh);
LONG lh_arc_seek(BPTR fh, LONG position, LONG mode);

LONG lh_arc_name_from_lock(BPTR lock, STRPTR buffer, LONG len);

LONG lh_arc_add_entry(struct LhArchive *archive, STRPTR name, APTR data, LONG len);
LONG lh_arc_add_entry_taglist(struct LhArchive *archive, STRPTR name, APTR data,
    LONG len, struct TagItem *tags);
LONG lh_arc_delete_entry(struct LhArchive *archive, STRPTR name);
LONG lh_arc_concat(struct LhArchive *dest, STRPTR source_path);
LONG lh_arc_set_password(struct LhArchive *archive, STRPTR password);

#endif /* LH_ARCHIVE_DOS_H */
