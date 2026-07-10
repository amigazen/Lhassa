/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2011, 2012, Simon Howard
 * Copyright (c) 2026 amigazen project
 *
 * lh_lh1dec.c - -lh1- adaptive Huffman + 4 KiB LZSS (LHarc 1.x).
 *
 * Ported from lhasa lh1_decoder.c to C89 for Amiga/SAS-C.  Decoder state
 * is malloc'd (several KiB of tree nodes) so it never sits on the stack.
 */

#include "lh_internal.h"

#include <string.h>

#define LH1_RING_SIZE       4096
#define LH1_TREE_REORDER    (32 * 1024)
#define LH1_NUM_CODES       314
#define LH1_NUM_TREE_NODES  (LH1_NUM_CODES * 2 - 1)
#define LH1_NUM_OFFSETS     64
#define LH1_MIN_OFFSET_LEN  3
#define LH1_COPY_THRESHOLD  3

struct lh1_node {
    unsigned char leaf;
    unsigned short child_index;
    unsigned short parent;
    unsigned short freq;
    unsigned short group;
};

struct lh1_bitreader {
    const unsigned char *in;
    unsigned long in_size;
    unsigned long in_pos;
    unsigned long bit_buffer;
    unsigned int bits;
};

struct lh1_dec {
    struct lh1_bitreader br;
    unsigned char ringbuf[LH1_RING_SIZE];
    unsigned int ringbuf_pos;
    struct lh1_node nodes[LH1_NUM_TREE_NODES];
    unsigned short leaf_nodes[LH1_NUM_CODES];
    unsigned short groups[LH1_NUM_TREE_NODES];
    unsigned int num_groups;
    unsigned short group_leader[LH1_NUM_TREE_NODES];
    unsigned char offset_lookup[256];
    unsigned char offset_lengths[LH1_NUM_OFFSETS];
};

static const unsigned int lh1_offset_fdist[] = {
    1, 3, 8, 12, 24, 16
};

static void lh1_br_init(struct lh1_bitreader *br,
    const unsigned char *in, unsigned long in_size)
{
    br->in = in;
    br->in_size = in_size;
    br->in_pos = 0;
    br->bit_buffer = 0;
    br->bits = 0;
}

static int lh1_peek_bits(struct lh1_bitreader *br, unsigned int n)
{
    unsigned char buf[4];
    unsigned int fill_bytes;
    unsigned long bytes;
    unsigned long i;

    if (n == 0) {
        return 0;
    }
    while (br->bits < n) {
        fill_bytes = (32 - br->bits) / 8;
        bytes = 0;
        while (bytes < fill_bytes && br->in_pos < br->in_size) {
            buf[bytes++] = br->in[br->in_pos++];
        }
        if (bytes == 0) {
            return -1;
        }
        for (i = 0; i < bytes; i++) {
            br->bit_buffer |= ((unsigned long)buf[i]) << (24 - br->bits);
            br->bits += 8;
        }
    }
    return (int)(br->bit_buffer >> (32 - n));
}

static int lh1_read_bits(struct lh1_bitreader *br, unsigned int n)
{
    int result;

    result = lh1_peek_bits(br, n);
    if (result >= 0) {
        br->bit_buffer <<= n;
        br->bits -= n;
    }
    return result;
}

static int lh1_read_bit(struct lh1_bitreader *br)
{
    return lh1_read_bits(br, 1);
}

static unsigned short lh1_alloc_group(struct lh1_dec *d)
{
    unsigned short result;

    result = d->groups[d->num_groups];
    d->num_groups++;
    return result;
}

static void lh1_free_group(struct lh1_dec *d, unsigned short group)
{
    d->num_groups--;
    d->groups[d->num_groups] = group;
}

static void lh1_init_groups(struct lh1_dec *d)
{
    unsigned int i;

    for (i = 0; i < LH1_NUM_TREE_NODES; i++) {
        d->groups[i] = (unsigned short)i;
    }
    d->num_groups = 0;
}

static void lh1_init_tree(struct lh1_dec *d)
{
    unsigned int i;
    unsigned int child;
    int node_index;
    unsigned short leaf_group;
    struct lh1_node *node;

    node_index = LH1_NUM_TREE_NODES - 1;
    leaf_group = lh1_alloc_group(d);

    for (i = 0; i < LH1_NUM_CODES; i++) {
        node = &d->nodes[node_index];
        node->leaf = 1;
        node->child_index = (unsigned short)i;
        node->freq = 1;
        node->group = leaf_group;
        d->group_leader[leaf_group] = (unsigned short)node_index;
        d->leaf_nodes[i] = (unsigned short)node_index;
        node_index--;
    }

    child = LH1_NUM_TREE_NODES - 1;
    while (node_index >= 0) {
        node = &d->nodes[node_index];
        node->leaf = 0;
        node->child_index = (unsigned short)child;
        d->nodes[child].parent = (unsigned short)node_index;
        d->nodes[child - 1].parent = (unsigned short)node_index;
        node->freq = (unsigned short)(d->nodes[child].freq
            + d->nodes[child - 1].freq);
        if (node->freq == d->nodes[node_index + 1].freq) {
            node->group = d->nodes[node_index + 1].group;
        } else {
            node->group = lh1_alloc_group(d);
        }
        d->group_leader[node->group] = (unsigned short)node_index;
        node_index--;
        child -= 2;
    }
}

static void lh1_fill_offset_range(struct lh1_dec *d, unsigned char code,
    unsigned int mask, unsigned int offset)
{
    unsigned int i;

    for (i = 0; (i & ~mask) == 0; i++) {
        d->offset_lookup[code | i] = (unsigned char)offset;
    }
}

static void lh1_init_offset_table(struct lh1_dec *d)
{
    unsigned int i;
    unsigned int j;
    unsigned int len;
    unsigned char code;
    unsigned char iterbit;
    unsigned char offset;

    code = 0;
    offset = 0;
    for (i = 0; i < (unsigned int)(sizeof(lh1_offset_fdist) / sizeof(lh1_offset_fdist[0])); i++) {
        len = i + LH1_MIN_OFFSET_LEN;
        iterbit = (unsigned char)(1 << (8 - len));
        for (j = 0; j < lh1_offset_fdist[i]; j++) {
            lh1_fill_offset_range(d, code, (unsigned int)(iterbit - 1), offset);
            d->offset_lengths[offset] = (unsigned char)len;
            code = (unsigned char)(code + iterbit);
            offset++;
        }
    }
}

static void lh1_init_ring(struct lh1_dec *d)
{
    memset(d->ringbuf, ' ', LH1_RING_SIZE);
    d->ringbuf_pos = 0;
}

static unsigned short lh1_make_group_leader(struct lh1_dec *d,
    unsigned short node_index)
{
    struct lh1_node *node;
    struct lh1_node *leader;
    unsigned short group;
    unsigned short leader_index;
    unsigned int tmp;

    group = d->nodes[node_index].group;
    leader_index = d->group_leader[group];
    if (leader_index == node_index) {
        return node_index;
    }

    node = &d->nodes[node_index];
    leader = &d->nodes[leader_index];

    tmp = leader->leaf;
    leader->leaf = node->leaf;
    node->leaf = (unsigned char)tmp;

    tmp = leader->child_index;
    leader->child_index = node->child_index;
    node->child_index = (unsigned short)tmp;

    if (node->leaf) {
        d->leaf_nodes[node->child_index] = node_index;
    } else {
        d->nodes[node->child_index].parent = node_index;
        d->nodes[node->child_index - 1].parent = node_index;
    }
    if (leader->leaf) {
        d->leaf_nodes[leader->child_index] = leader_index;
    } else {
        d->nodes[leader->child_index].parent = leader_index;
        d->nodes[leader->child_index - 1].parent = leader_index;
    }
    return leader_index;
}

static void lh1_increment_node_freq(struct lh1_dec *d, unsigned short node_index)
{
    struct lh1_node *node;
    struct lh1_node *other;

    node = &d->nodes[node_index];
    other = &d->nodes[node_index - 1];
    node->freq++;

    if (node_index < LH1_NUM_TREE_NODES - 1
        && node->group == d->nodes[node_index + 1].group) {
        d->group_leader[node->group]++;
        if (node->freq == other->freq) {
            node->group = other->group;
        } else {
            node->group = lh1_alloc_group(d);
            d->group_leader[node->group] = node_index;
        }
    } else {
        if (node->freq == other->freq) {
            lh1_free_group(d, node->group);
            node->group = other->group;
        }
    }
}

static void lh1_reconstruct_tree(struct lh1_dec *d)
{
    struct lh1_node *leaf;
    unsigned int child;
    unsigned int freq;
    unsigned int group;
    int i;

    leaf = d->nodes;
    for (i = 0; i < LH1_NUM_TREE_NODES; i++) {
        if (d->nodes[i].leaf) {
            leaf->leaf = 1;
            leaf->child_index = d->nodes[i].child_index;
            leaf->freq = (unsigned short)((d->nodes[i].freq + 1) / 2);
            leaf++;
        }
    }

    leaf = &d->nodes[LH1_NUM_CODES - 1];
    child = LH1_NUM_TREE_NODES - 1;
    i = LH1_NUM_TREE_NODES - 1;

    while (i >= 0) {
        while ((int)child - i < 2) {
            d->nodes[i] = *leaf;
            d->leaf_nodes[leaf->child_index] = (unsigned short)i;
            i--;
            leaf--;
        }
        freq = (unsigned int)(d->nodes[child].freq
            + d->nodes[child - 1].freq);
        while (leaf >= d->nodes && freq >= leaf->freq) {
            d->nodes[i] = *leaf;
            d->leaf_nodes[leaf->child_index] = (unsigned short)i;
            i--;
            leaf--;
        }
        d->nodes[i].leaf = 0;
        d->nodes[i].freq = (unsigned short)freq;
        d->nodes[i].child_index = (unsigned short)child;
        d->nodes[child].parent = (unsigned short)i;
        d->nodes[child - 1].parent = (unsigned short)i;
        i--;
        child -= 2;
    }

    lh1_init_groups(d);
    group = lh1_alloc_group(d);
    d->nodes[0].group = (unsigned short)group;
    d->group_leader[group] = 0;
    for (i = 1; i < LH1_NUM_TREE_NODES; i++) {
        if (d->nodes[i].freq == d->nodes[i - 1].freq) {
            d->nodes[i].group = d->nodes[i - 1].group;
        } else {
            group = lh1_alloc_group(d);
            d->nodes[i].group = (unsigned short)group;
            d->group_leader[group] = (unsigned short)i;
        }
    }
}

static void lh1_increment_for_code(struct lh1_dec *d, unsigned short code)
{
    unsigned short node_index;

    if (d->nodes[0].freq >= LH1_TREE_REORDER) {
        lh1_reconstruct_tree(d);
    }
    d->nodes[0].freq++;
    node_index = d->leaf_nodes[code];
    while (node_index != 0) {
        node_index = lh1_make_group_leader(d, node_index);
        lh1_increment_node_freq(d, node_index);
        node_index = d->nodes[node_index].parent;
    }
}

static int lh1_read_code(struct lh1_dec *d, unsigned short *result)
{
    unsigned int node_index;
    int bit;

    node_index = 0;
    while (!d->nodes[node_index].leaf) {
        bit = lh1_read_bit(&d->br);
        if (bit < 0) {
            return 0;
        }
        node_index = d->nodes[node_index].child_index - (unsigned int)bit;
    }
    *result = d->nodes[node_index].child_index;
    lh1_increment_for_code(d, *result);
    return 1;
}

static int lh1_read_offset(struct lh1_dec *d, unsigned int *result)
{
    unsigned int offset;
    int future;
    int offset2;

    future = lh1_peek_bits(&d->br, 8);
    if (future < 0) {
        return 0;
    }
    offset = d->offset_lookup[future];
    lh1_read_bits(&d->br, d->offset_lengths[offset]);
    offset2 = lh1_read_bits(&d->br, 6);
    if (offset2 < 0) {
        return 0;
    }
    *result = (offset << 6) | (unsigned int)offset2;
    return 1;
}

static void lh1_output_byte(struct lh1_dec *d, unsigned char *out,
    unsigned long *out_pos, unsigned char b)
{
    out[*out_pos] = b;
    (*out_pos)++;
    d->ringbuf[d->ringbuf_pos] = b;
    d->ringbuf_pos = (d->ringbuf_pos + 1) % LH1_RING_SIZE;
}

long lh_decompress_lh1(void *inBuf, unsigned long inSize, void *outBuf,
    unsigned long outSize)
{
    struct lh1_dec *d;
    unsigned char *out;
    unsigned long out_pos;
    unsigned short code;
    unsigned int offset;
    unsigned int count;
    unsigned int start;
    unsigned int i;
    unsigned int pos;

    if (!inBuf || !outBuf || inSize == 0 || outSize == 0) {
        return LH_ERR_INVALID_ARG;
    }

    d = (struct lh1_dec *)malloc(sizeof(*d));
    if (!d) {
        return LH_ERR_NO_MEMORY;
    }
    memset(d, 0, sizeof(*d));
    lh1_br_init(&d->br, (const unsigned char *)inBuf, inSize);
    lh1_init_groups(d);
    lh1_init_tree(d);
    lh1_init_offset_table(d);
    lh1_init_ring(d);

    out = (unsigned char *)outBuf;
    out_pos = 0;

    while (out_pos < outSize) {
        if (!lh1_read_code(d, &code)) {
            free(d);
            return LH_ERR_INVALID_ARCHIVE;
        }
        if (code < 0x100) {
            lh1_output_byte(d, out, &out_pos, (unsigned char)code);
        } else {
            if (!lh1_read_offset(d, &offset)) {
                free(d);
                return LH_ERR_INVALID_ARCHIVE;
            }
            count = (unsigned int)code - 0x100U + LH1_COPY_THRESHOLD;
            start = d->ringbuf_pos - offset + LH1_RING_SIZE - 1;
            for (i = 0; i < count; i++) {
                if (out_pos >= outSize) {
                    break;
                }
                pos = (start + i) % LH1_RING_SIZE;
                lh1_output_byte(d, out, &out_pos, d->ringbuf[pos]);
            }
        }
    }

    free(d);
    return LH_OK;
}
