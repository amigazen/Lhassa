/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh.h - Public API for Lhassa LHA archive read/write.
 */

#ifndef LH_H
#define LH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lh_status {
    LH_OK = 0,
    LH_ERR_IO,
    LH_ERR_TRUNCATED,
    LH_ERR_INVALID_ARCHIVE,
    LH_ERR_CRC_MISMATCH,
    LH_ERR_BAD_HEADER,
    LH_ERR_FILENAME_TOO_LONG,
    LH_ERR_NO_MEMORY,
    LH_ERR_INVALID_ARG,
    LH_ERR_UNSUPPORTED,
    LH_ERR_PASSWORD,
    LH_ERR_NOT_FOUND,
    LH_ERR_ABORTED
} lh_status;

typedef enum lh_method {
    LH_METHOD_LH0 = 0,
    LH_METHOD_LH1 = 1,
    LH_METHOD_LH2 = 2,
    LH_METHOD_LH3 = 3,
    LH_METHOD_LH4 = 4,
    LH_METHOD_LH5 = 5,
    LH_METHOD_LH6 = 6,
    LH_METHOD_LH7 = 7,
    LH_METHOD_LZS = 8,
    LH_METHOD_LZ5 = 9,
    LH_METHOD_LZ4 = 10,
    LH_METHOD_LHD = 11,
    LH_METHOD_LH8 = 12,
    LH_METHOD_LH9 = 13,
    LH_METHOD_LHA = 14,
    LH_METHOD_LHB = 15,
    LH_METHOD_LHC = 16,
    LH_METHOD_LHE = 17,
    LH_METHOD_PC1 = 18,
    LH_METHOD_PM0 = 19,
    LH_METHOD_PM1 = 20,
    LH_METHOD_PM2 = 21,
    LH_METHOD_PMS = 22,
    LH_METHOD_LZ2 = 23,
    LH_METHOD_LZ3 = 24,
    LH_METHOD_LZ7 = 25,
    LH_METHOD_LZ8 = 26,
    LH_METHOD_LHX = 27
} lh_method;

typedef enum lh_level {
    LH_LEVEL_STORE = 0,
    LH_LEVEL_LH1 = 1,
    LH_LEVEL_LH5 = 5,
    LH_LEVEL_LH6 = 6,
    LH_LEVEL_LH7 = 7
} lh_level;

/* LHA packed DOS date/time (same bit layout as MS-DOS). */
typedef struct lh_datetime {
    unsigned short year;
    unsigned char month;
    unsigned char day;
    unsigned char hour;
    unsigned char minute;
    unsigned char second;
} lh_datetime;

/* Amiga protection byte (PSHAEDWR); default 0x0f (----rwed). */
typedef unsigned char lh_attrs;

#define LH_ATTR_DEFAULT ((lh_attrs)0x0f)

typedef struct lh_entry {
    char *filename;
    char *comment;
    lh_attrs attrs;
    lh_datetime datetime;
    unsigned char *data;
    size_t data_len;
    size_t packed_len;
    lh_method method;
    unsigned char os_id;
    unsigned char header_level;
    unsigned short crc;
    int crc_ok;
    int is_directory;
    int is_encrypted;
} lh_entry;

typedef struct lh_writer lh_writer;
typedef struct lh_reader lh_reader;

/* In-memory buffer API (lh.library compatible shape). */
typedef struct LhBuffer {
    void *lh_Src;
    unsigned long lh_SrcSize;
    void *lh_Dst;
    unsigned long lh_DstSize;
    void *lh_Aux;
    unsigned long lh_AuxSize;
    unsigned long lh_Reserved;
} LhBuffer;

#define LH_ENCODE_EXTRA(n) (((n) + 7u) >> 3)

const char *lh_status_string(lh_status st);
void lh_set_debug_verbose(int on);

/* --- datetime helpers --- */

lh_status lh_datetime_pack(const lh_datetime *dt, unsigned long *out);
void lh_datetime_unpack(unsigned long packed, lh_datetime *dt);
lh_status lh_datetime_validate(const lh_datetime *dt);
void lh_datetime_from_time_t(lh_datetime *dt, long t);

/* --- method helpers --- */

lh_method lh_method_from_string(const char *sig);
const char *lh_method_to_string(lh_method method);
lh_level lh_level_default(void);

/* --- writer --- */

lh_writer *lh_writer_open(const char *path, unsigned char header_level,
    lh_level def_level, lh_status *err);
lh_writer *lh_writer_open_append(const char *path, lh_status *err);
lh_status lh_writer_add(
    lh_writer *w,
    const char *filename,
    const char *comment,
    lh_attrs attrs,
    const lh_datetime *datetime,
    lh_level level,
    int store_only,
    const unsigned char *data,
    size_t data_len
);
lh_status lh_writer_close(lh_writer **w);

/* --- reader --- */

lh_reader *lh_reader_open(const char *path, lh_status *err);
lh_status lh_reader_set_password(lh_reader *r, const char *password);

typedef void (*lh_progress_fn)(void *ctx, size_t done, size_t total);
typedef void (*lh_file_begin_fn)(void *ctx, const char *name, size_t size);
typedef void (*lh_file_end_fn)(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size);

void lh_reader_set_progress(
    lh_reader *r,
    lh_file_begin_fn on_begin,
    lh_progress_fn on_progress,
    lh_file_end_fn on_end,
    void *ctx
);

void lh_reader_set_header_only(lh_reader *r, int on);

int lh_reader_archive_datetime(const lh_reader *r, lh_datetime *dt);

lh_status lh_reader_next(lh_reader *r, lh_entry *entry);
void lh_reader_close(lh_reader **r);
void lh_entry_clear(lh_entry *entry);

typedef int (*lh_keep_fn)(const lh_entry *entry, void *ctx);

lh_status lh_archive_rewrite(
    const char *archive,
    lh_keep_fn keep,
    void *ctx,
    unsigned char header_level,
    lh_level level,
    int store_only
);

/* --- low-level codec --- */

lh_status lh_compress(
    lh_method method,
    const unsigned char *plain,
    size_t plain_len,
    unsigned char *out,
    size_t *out_len
);

lh_status lh_decompress(
    lh_method method,
    const unsigned char *compressed,
    size_t compressed_len,
    size_t expected_len,
    unsigned char *out,
    size_t out_cap
);

/* --- lh.library buffer API --- */

LhBuffer *lh_create_buffer(int only_decode);
void lh_delete_buffer(LhBuffer *buf);
unsigned long lh_encode(LhBuffer *buf);
unsigned long lh_decode(LhBuffer *buf);

#ifdef __cplusplus
}
#endif

#endif /* LH_H */
