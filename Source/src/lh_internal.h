/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_internal.h - Private declarations shared by library sources.
 */

#ifndef LH_INTERNAL_H
#define LH_INTERNAL_H

#include "lh_platform.h"
#include <string.h>

#ifdef LH_AMIGA
#include "/include/lh.h"
#else
#include "lh.h"
#endif

#ifdef LH_AMIGA
#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef DOS_DOS_H
#include <dos/dos.h>
#endif

/* Pooled allocator from lib_source/malloc.c (not libc). */
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t num, size_t size);
void *realloc(void *ptr, size_t size);
#elif defined(LH_HOST)
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#else
#error "lh core: LH_AMIGA (library) or LH_HOST (make HOST=1) required"
#endif

typedef unsigned char lh_u8;
typedef unsigned short lh_u16;
typedef unsigned long lh_u32;

#define LH_SIG_LEN 5

#define LH_SIG_LH0 "-lh0-"
#define LH_SIG_LH1 "-lh1-"
#define LH_SIG_LH2 "-lh2-"
#define LH_SIG_LH3 "-lh3-"
#define LH_SIG_LH4 "-lh4-"
#define LH_SIG_LH5 "-lh5-"
#define LH_SIG_LH6 "-lh6-"
#define LH_SIG_LH7 "-lh7-"
#define LH_SIG_LZS "-lzs-"
#define LH_SIG_LZ5 "-lz5-"
#define LH_SIG_LZ4 "-lz4-"
#define LH_SIG_LHD "-lhd-"

#define LH_OS_AMIGA 'A'
#define LH_OS_MSDOS 'M'

#define LH_BUF_DECODE_AUX 4500u
#define LH_BUF_ENCODE_AUX 40000u

/* Huffman tree limits */
#define LH_MAX_HUFFMAN_CODE 16
#define LH_MAX_HUFFMAN_LEAVES 512

#define LH_RING_BUFFER_SIZE 4096
#define LH_POSITION_BITS 12
#define LH_MATCH_LENGTH_THRESHOLD 3
#define LH_MAX_MATCH_LENGTH 256

#define MATCH_LENGTH_THRESHOLD LH_MATCH_LENGTH_THRESHOLD
#define POSITION_BITS LH_POSITION_BITS
#define MAX_MATCH_LENGTH LH_MAX_MATCH_LENGTH

struct lh_huffman_node {
    unsigned short child[2];
    unsigned short value;
    unsigned short freq;
};

struct lh_decompress_state {
    unsigned char *inBuffer;
    unsigned long inSize;
    unsigned long inPos;
    unsigned long bitBuffer;
    unsigned long bitCount;
    unsigned char *outBuffer;
    unsigned long outSize;
    unsigned long outPos;
    unsigned char ringBuffer[LH_RING_BUFFER_SIZE + LH_MAX_MATCH_LENGTH - 1];
    unsigned long ringPos;
    struct lh_huffman_node codeTree[LH_MAX_HUFFMAN_LEAVES];
    struct lh_huffman_node lengthTree[LH_MAX_HUFFMAN_LEAVES];
    unsigned short codeTable[256];
    unsigned short lengthTable[256];
};

struct lh_code_lookup {
    unsigned short code;
    unsigned char codeLen;
    unsigned long freq;
};

/* CRC */
unsigned short lh_crc16(const unsigned char *data, size_t len);
unsigned short lh_crc16_update(unsigned short crc, unsigned char b);

/* Crypto */
typedef struct lh_decrypt_state {
    unsigned long keys[3];
    unsigned char last_char;
    int initialized;
} lh_decrypt_state;

int lh_decrypt_init(lh_decrypt_state *st, const char *password);
void lh_decrypt_buffer(lh_decrypt_state *st, unsigned char *buf, size_t len);

/* Datetime */
unsigned long lh_dos_timestamp_pack(const lh_datetime *dt);
void lh_datetime_now(lh_datetime *dt);
#ifdef LH_AMIGA
void lh_datetime_from_datestamp(const struct DateStamp *ds, lh_datetime *dt);
#endif

/* Codec internals */
void lh_init_decompress_state(
    struct lh_decompress_state *state,
    void *in_buf, unsigned long in_size,
    void *out_buf, unsigned long out_size
);
unsigned long lh_read_bits(struct lh_decompress_state *state, unsigned long num_bits);
void lh_build_huffman_tree(
    struct lh_huffman_node *tree,
    unsigned char *lengths,
    unsigned long num_symbols
);
unsigned long lh_decode_value(
    struct lh_decompress_state *state,
    struct lh_huffman_node *tree,
    unsigned long num_symbols
);

long lh_codec_decompress(
    void *in_buf, unsigned long in_size,
    void *out_buf, unsigned long out_size,
    long method
);
long lh_codec_compress(
    void *in_buf, unsigned long in_size,
    void *out_buf, unsigned long *out_size,
    long method
);

long lh_decompress_lh0(void *, unsigned long, void *, unsigned long);
long lh_compress_lh0(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh1(void *, unsigned long, void *, unsigned long);
long lh_compress_lh1(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh2(void *, unsigned long, void *, unsigned long);
long lh_compress_lh2(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh3(void *, unsigned long, void *, unsigned long);
long lh_compress_lh3(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh4(void *, unsigned long, void *, unsigned long);
long lh_compress_lh4(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh5(void *, unsigned long, void *, unsigned long);
long lh_compress_lh5(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh6(void *, unsigned long, void *, unsigned long);
long lh_compress_lh6(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lh7(void *, unsigned long, void *, unsigned long);
long lh_compress_lh7(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lzs(void *, unsigned long, void *, unsigned long);
long lh_compress_lzs(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lz5(void *, unsigned long, void *, unsigned long);
long lh_compress_lz5(void *, unsigned long, void *, unsigned long *);
long lh_decompress_lz4(void *, unsigned long, void *, unsigned long);
long lh_compress_lz4(void *, unsigned long, void *, unsigned long *);

/* Secret CLI debug (-v): header/position tracing on stderr. */
extern int lh_debug_verbose;
void lh_set_debug_verbose(int on);

/* Header */
typedef struct lh_hdr_meta {
    char method_sig[LH_SIG_LEN + 1];
    lh_method method;
    unsigned long packed_size;
    unsigned long original_size;
    unsigned long timestamp;
    unsigned char attribute;
    unsigned char header_level;
    unsigned char os_id;
    unsigned short crc;
    int has_crc;
    int is_directory;
    char *filename;
    char *comment;
} lh_hdr_meta;

void lh_hdr_meta_clear(lh_hdr_meta *m);

/*
 * lh_stream - dos.library file I/O (Open/Read/Write/Seek/Close per dos.doc).
 */
typedef struct lh_stream lh_stream;

struct lh_stream {
#ifdef LH_AMIGA
    BPTR lh_fh;
#elif defined(LH_HOST)
    FILE *lh_fp;
#endif
};

void lh_stream_init(lh_stream *s);

#ifdef LH_AMIGA
#define LH_STREAM_OPEN(s) ((s)->lh_fh != 0)
#elif defined(LH_HOST)
#define LH_STREAM_OPEN(s) ((s)->lh_fp != NULL)
#endif

int lh_stream_open_read(lh_stream *s, const char *path);
int lh_stream_open_write(lh_stream *s, const char *path);
int lh_stream_open_append(lh_stream *s, const char *path);
int lh_stream_file_exists(const char *path);
void lh_stream_close(lh_stream *s);
size_t lh_stream_read(lh_stream *s, void *buf, size_t n);
size_t lh_stream_write(lh_stream *s, const void *buf, size_t n);
int lh_stream_seek_cur(lh_stream *s, long delta);
long lh_stream_tell(lh_stream *s);

lh_status lh_hdr_read(lh_stream *io, lh_hdr_meta *meta, unsigned char write_level);
lh_status lh_hdr_write(
    lh_stream *io,
    const lh_hdr_meta *meta,
    unsigned char write_level
);

lh_u32 lh_read_le32(const unsigned char *p);
void lh_write_le32(unsigned char *p, lh_u32 v);
lh_u16 lh_read_le16(const unsigned char *p);
void lh_write_le16(unsigned char *p, lh_u16 v);

#endif /* LH_INTERNAL_H */
