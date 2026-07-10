/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_codec.c - LHA compression and decompression methods.
 */

#include "lh_internal.h"

/* Global variables for bit buffer (lh6/lh7 encode pass) */
static unsigned long bitBuffer = 0;
static unsigned long bitCount = 0;

/* Write bits into encode output; returns LH_OK or LH_ERR_NO_MEMORY. */
static long lh_encode_write_bits(
    unsigned long bits,
    unsigned long num_bits,
    unsigned long *enc_bit_buf,
    unsigned long *enc_bit_cnt,
    unsigned char *output,
    unsigned long *out_pos,
    unsigned long out_cap)
{
    *enc_bit_buf |= (bits << *enc_bit_cnt);
    *enc_bit_cnt += num_bits;
    while (*enc_bit_cnt >= 8) {
        if (*out_pos >= out_cap) {
            return LH_ERR_NO_MEMORY;
        }
        output[(*out_pos)++] = (unsigned char)(*enc_bit_buf & 0xFF);
        *enc_bit_buf >>= 8;
        *enc_bit_cnt -= 8;
    }
    return LH_OK;
}

/* Constants for LH7 decompression */
#define LH7_DICT_SIZE 8192      /* 8K dictionary size */
#define LH7_MAX_MATCH 256       /* Maximum match length */
#define LH7_OFFSET_BITS 13      /* Bits for offset */
#define LH7_LITERAL_CODES 256   /* Literal codes */
#define LH7_LENGTH_CODES 32     /* Length codes */
#define LH7_TOTAL_CODES (LH7_LITERAL_CODES + LH7_LENGTH_CODES)

/* Constants for LH6 decompression */
#define LH6_DICT_SIZE 32768     /* 32K dictionary size */
#define LH6_MAX_MATCH 256       /* Maximum match length */
#define LH6_OFFSET_BITS 15      /* Bits for offset */
#define LH6_LITERAL_CODES 256   /* Literal codes */
#define LH6_LENGTH_CODES 16     /* Length codes */
#define LH6_TOTAL_CODES (LH6_LITERAL_CODES + LH6_LENGTH_CODES)

/* Constants for LZS decompression */
#define LZS_WINDOW_SIZE 2048     /* 2K sliding window */
#define LZS_BREAK_EVEN 3         /* Minimum match length */
#define LZS_MAX_MATCH 256        /* Maximum match length */

/* Initialize decompression state structure */
void lh_init_decompress_state(struct lh_decompress_state *state, void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize) {
    /* Declare all variables at the start */
    unsigned char *inPtr;
    unsigned char *outPtr;
    
    if (!state || !inBuf || !outBuf) {
        return;
    }
    
    /* Initialize input buffer */
    inPtr = (unsigned char *)inBuf;
    state->inBuffer = inPtr;
    state->inSize = inSize;
    state->inPos = 0;
    state->bitBuffer = 0;
    state->bitCount = 0;
    
    /* Initialize output buffer */
    outPtr = (unsigned char *)outBuf;
    state->outBuffer = outPtr;
    state->outSize = outSize;
    state->outPos = 0;
    
    /* Initialize ring buffer */
    memset(state->ringBuffer, ' ', LH_RING_BUFFER_SIZE + LH_MAX_MATCH_LENGTH - 1);
    state->ringPos = LH_RING_BUFFER_SIZE - LH_MAX_MATCH_LENGTH;
    
    /* Initialize Huffman trees */
    memset(state->codeTree, 0, sizeof(state->codeTree));
    memset(state->lengthTree, 0, sizeof(state->lengthTree));
    memset(state->codeTable, 0, sizeof(state->codeTable));
    memset(state->lengthTable, 0, sizeof(state->lengthTable));
}

/* Read bits from the input stream */
unsigned long lh_read_bits(struct lh_decompress_state *state, unsigned long numBits) {
    /* Declare all variables at the start */
    unsigned long result = 0;
    unsigned long bitsNeeded;
    unsigned char byte;
    
    /* Ensure we have enough bits in the buffer */
    while (state->bitCount < numBits) {
        /* Check if we're at the end of the input buffer */
        if (state->inPos >= state->inSize) {
            return 0;
        }
        
        /* Read next byte and add to bit buffer */
        byte = state->inBuffer[state->inPos++];
        state->bitBuffer |= ((unsigned long)byte) << state->bitCount;
        state->bitCount += 8;
    }
    
    /* Extract the requested bits */
    result = state->bitBuffer & ((1UL << numBits) - 1);
    
    /* Remove used bits from buffer */
    state->bitBuffer >>= numBits;
    state->bitCount -= numBits;
    
    return result;
}

/* Build a Huffman tree from code lengths */
void lh_build_huffman_tree(struct lh_huffman_node *tree, unsigned char *lengths, unsigned long numSymbols) {
    unsigned short count[LH_MAX_HUFFMAN_CODE + 1];
    unsigned short code = 0;
    unsigned short codeOffsets[LH_MAX_HUFFMAN_CODE + 1];
    unsigned long i, len;
    unsigned short nodeIndex = 0;
    unsigned short p = 0;
    unsigned long j;
    unsigned short bit;
    
    /* Count the number of codes of each length */
    memset(count, 0, sizeof(count));
    for (i = 0; i < numSymbols; i++) {
        if (lengths[i] > 0) {
            count[lengths[i]]++;
        }
    }
    
    /* Compute the starting code for each length */
    memset(codeOffsets, 0, sizeof(codeOffsets));
    for (i = 1; i <= LH_MAX_HUFFMAN_CODE; i++) {
        code = (code + count[i-1]) << 1;
        codeOffsets[i] = code;
    }
    
    /* Assign codes to symbols and build tree */
    memset(tree, 0, LH_MAX_HUFFMAN_LEAVES * sizeof(struct lh_huffman_node));
    nodeIndex = 1; /* Root node is at index 0 */
    
    for (i = 0; i < numSymbols; i++) {
        len = lengths[i];
        if (len > 0) {
            code = codeOffsets[len]++;
                        
            /* Process the code bit by bit, starting from MSB */
            for (j = 0; j < len - 1; j++) {
                bit = (code >> (len - j - 1)) & 1;
                
                /* If child doesn't exist, create it */
                if (tree[p].child[bit] == 0) {
                    tree[p].child[bit] = (unsigned short)(nodeIndex + numSymbols);
                    nodeIndex++;
                }

                /* Move to child */
                p = (unsigned short)(tree[p].child[bit] - numSymbols);
            }
            
            /* Process the last bit */
            bit = code & 1;
            tree[p].child[bit] = i;
        }
    }
}

/* Decode a value using a Huffman tree (leaves < num_symbols, internals offset). */
unsigned long lh_decode_value(
    struct lh_decompress_state *state,
    struct lh_huffman_node *tree,
    unsigned long num_symbols)
{
    unsigned short p = 0;
    unsigned long bit;
    unsigned short child;

    while (1) {
        bit = lh_read_bits(state, 1);
        child = tree[p].child[bit];
        if (child == 0) {
            return 0;
        }
        if ((unsigned long)child < num_symbols) {
            return (unsigned long)child;
        }
        p = (unsigned short)(child - num_symbols);
    }
}

/* Decompress a buffer using specified method */
long lh_codec_decompress(void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize, long method) {
    switch (method) {
        case LH_METHOD_LH0:
            return lh_decompress_lh0(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH1:
            return lh_decompress_lh1(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH2:
            return lh_decompress_lh2(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH3:
            return lh_decompress_lh3(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH4:
            return lh_decompress_lh4(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH5:
            return lh_decompress_lh5(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH6:
            return lh_decompress_lh6(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LH7:
            return lh_decompress_lh7(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LZS:
            return lh_decompress_lzs(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LZ5:
            return lh_decompress_lz5(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LZ4:
            return lh_decompress_lz4(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LHD:
            return LH_OK; /* Directory entries have no data to decompress */
        case LH_METHOD_LH8:
        case LH_METHOD_LH9:
        case LH_METHOD_LHA:
        case LH_METHOD_LHB:
        case LH_METHOD_LHC:
        case LH_METHOD_LHE:
            /* Joe Jared extensions - use LH7 as base for now */
            return lh_decompress_lh7(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_PC1:
        case LH_METHOD_PM0:
            /* PMarc store — same as LH0. */
            return lh_decompress_lh0(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_PM1:
        case LH_METHOD_PM2:
            /*
             * PMarc CP/M compressors — not Amiga LHA.  Do not feed these
             * bitstreams into LH5; that crashed on pmarc124/mtcd.pma.
             */
            return LH_ERR_UNSUPPORTED;
        case LH_METHOD_PMS:
            /* PMarc self-extracting stub — not supported. */
            return LH_ERR_UNSUPPORTED;
        case LH_METHOD_LZ2:
        case LH_METHOD_LZ3:
        case LH_METHOD_LZ7:
        case LH_METHOD_LZ8:
            /* LArc extensions - use LZS as base for now */
            return lh_decompress_lzs(inBuf, inSize, outBuf, outSize);
        case LH_METHOD_LHX:
            return lh_decompress_lhx(inBuf, inSize, outBuf, outSize);
        default:
            return LH_ERR_INVALID_ARG;
    }
}

/* Compress a buffer using specified method */
long lh_codec_compress(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize, long method) {
    /* Declare all variables at the start */
    long result;
    unsigned char *input;
    unsigned char *output;
    unsigned long size;
    
    /* Validate parameters */
    if (!inBuf || !outBuf || !outSize || inSize == 0) {
        return LH_ERR_INVALID_ARG;
    }
    
    input = (unsigned char *)inBuf;
    output = (unsigned char *)outBuf;
    size = *outSize;
    
    /* Select compression method */
    switch (method) {
        case LH_METHOD_LH0:
            result = lh_compress_lh0(input, inSize, output, &size);
            break;
        case LH_METHOD_LH1:
            result = lh_compress_lh1(input, inSize, output, &size);
            break;
        case LH_METHOD_LH2:
            result = lh_compress_lh2(input, inSize, output, &size);
            break;
        case LH_METHOD_LH3:
            result = lh_compress_lh3(input, inSize, output, &size);
            break;
        case LH_METHOD_LH4:
            result = lh_compress_lh4(input, inSize, output, &size);
            break;
        case LH_METHOD_LH5:
            result = lh_compress_lh5(input, inSize, output, &size);
            break;
        case LH_METHOD_LH6:
            result = lh_compress_lh6(input, inSize, output, &size);
            break;
        case LH_METHOD_LH7:
            result = lh_compress_lh7(input, inSize, output, &size);
            break;
        case LH_METHOD_LZS:
            result = lh_compress_lzs(input, inSize, output, &size);
            break;
        case LH_METHOD_LZ5:
            result = lh_compress_lz5(input, inSize, output, &size);
            break;
        case LH_METHOD_LZ4:
            result = lh_compress_lz4(input, inSize, output, &size);
            break;
        case LH_METHOD_LHD:
            result = LH_OK; /* Directory entries have no data to compress */
            size = 0;
            break;
        case LH_METHOD_LH8:
        case LH_METHOD_LH9:
        case LH_METHOD_LHA:
        case LH_METHOD_LHB:
        case LH_METHOD_LHC:
        case LH_METHOD_LHE:
            /* Joe Jared extensions - use LH7 as base for now */
            result = lh_compress_lh7(input, inSize, output, &size);
            break;
        case LH_METHOD_PC1:
        case LH_METHOD_PM0:
            /* PMarc store — same as LH0. */
            result = lh_compress_lh0(input, inSize, output, &size);
            break;
        case LH_METHOD_PM1:
        case LH_METHOD_PM2:
        case LH_METHOD_PMS:
            /* PMarc compress / SFX — intentionally unsupported. */
            result = LH_ERR_UNSUPPORTED;
            break;
        case LH_METHOD_LZ2:
        case LH_METHOD_LZ3:
        case LH_METHOD_LZ7:
        case LH_METHOD_LZ8:
            /* LArc extensions - use LZS as base for now */
            result = lh_compress_lzs(input, inSize, output, &size);
            break;
        case LH_METHOD_LHX:
            /* UNLHA32 extension - use LH7 as base for now */
            result = lh_compress_lh7(input, inSize, output, &size);
            break;
        default:
            result = LH_ERR_INVALID_ARG;
            break;
    }
    
    if (result == LH_OK) {
        *outSize = size;
    }
    
    return result;
}

/* LH0 (Store) - No compression, just copy */
long lh_compress_lh0(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    /* Declare all variables at the start */
    unsigned char *input;
    unsigned char *output;
    unsigned long i;
    
    /* Validate parameters */
    if (!inBuf || !outBuf || !outSize || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    input = (unsigned char *)inBuf;
    output = (unsigned char *)outBuf;
    
    /* Copy data */
    for (i = 0; i < inSize; i++) {
        output[i] = input[i];
    }
    
    *outSize = inSize;
    return LH_OK;
}

/* LH0 decompress - store, plain copy */
long lh_decompress_lh0(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    unsigned char *input;
    unsigned char *output;
    unsigned long i;

    if (!inBuf || !outBuf || inSize == 0 || outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    input = (unsigned char *)inBuf;
    output = (unsigned char *)outBuf;
    for (i = 0; i < inSize; i++) {
        output[i] = input[i];
    }
    return LH_OK;
}

/* lh_decompress_lh1 is implemented in lh_lh1dec.c. */

/* LH1 compression - LZSS with static Huffman coding */
long lh_compress_lh1(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long i;
    unsigned char window[LH_RING_BUFFER_SIZE];
    unsigned long windowPos = 0;
    unsigned long literalCount = 0;
    struct lh_code_lookup *codeLookup = NULL;
    struct lh_code_lookup *distLookup = NULL;
    long result = LH_OK;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough - worst case is input size + 1% + 12 bytes */
    if (*outSize < inSize + (inSize / 100) + 12) {
        return LH_ERR_NO_MEMORY;
    }
    
    /* Heap tables: distLookup alone is ~32 KiB (exceeds m68k stack displacement). */
    codeLookup = (struct lh_code_lookup *)malloc(
        (256u + LH_MAX_MATCH_LENGTH) * sizeof(struct lh_code_lookup));
    if (!codeLookup) {
        return LH_ERR_NO_MEMORY;
    }
    distLookup = (struct lh_code_lookup *)malloc(
        (LH_RING_BUFFER_SIZE + 1u) * sizeof(struct lh_code_lookup));
    if (!distLookup) {
        free(codeLookup);
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize the ring buffer with spaces */
    memset(window, ' ', LH_RING_BUFFER_SIZE);
    
    /* First pass: Build frequency tables by scanning input */
    memset(codeLookup, 0, (256u + LH_MAX_MATCH_LENGTH) * sizeof(struct lh_code_lookup));
    memset(distLookup, 0, (LH_RING_BUFFER_SIZE + 1u) * sizeof(struct lh_code_lookup));
    
    /* Simplistic first implementation - just encode all literals */
    /* In a real implementation, we would use a proper LZSS match finder */
    for (i = 0; i < inSize; i++) {
        codeLookup[input[i]].freq++;
        literalCount++;
    }
    
    /* Build simple Huffman codes for literals only (for now) */
    /* In real implementation we would build optimal codes */
    for (i = 0; i < 256; i++) {
        if (codeLookup[i].freq > 0) {
            codeLookup[i].codeLen = 8; /* Fixed 8-bit code for now */
            codeLookup[i].code = i;    /* Just use byte value as code */
        }
    }
    
    /* Write literal/length code table */
    for (i = 0; i < 256; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = 5; /* 5 bits per code for simplicity */
    }
    
    /* Write distance code table */
    for (i = 0; i < 256; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = 5; /* 5 bits per distance */
    }
    
    /* Encode data - just literals for now */
    for (i = 0; i < inSize; i++) {
        /* Write literal byte */
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = input[i];
        
        /* Update window */
        window[windowPos++] = input[i];
        if (windowPos >= LH_RING_BUFFER_SIZE) {
            windowPos = 0;
        }
    }
    
    /* Update output size */
    *outSize = outPos;
    
cleanup:
    if (distLookup) {
        free(distLookup);
    }
    if (codeLookup) {
        free(codeLookup);
    }
    return result;
}

long lh_compress_lh5(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long i;
    unsigned char *window = NULL;
    unsigned long windowPos = 0;
    unsigned long hashTable[256];
    unsigned long *hashLinks = NULL;
    const unsigned long NUM_LITERAL_CODES = 256;
    const unsigned long NUM_LEN_CODES = 16;
    const unsigned long NUM_CODES = NUM_LITERAL_CODES + NUM_LEN_CODES;
    const unsigned long DISTANCE_TABLE_SIZE = 64;
    unsigned long startPos = 0;
    unsigned long lookahead = (inSize < MATCH_LENGTH_THRESHOLD) ? inSize : MATCH_LENGTH_THRESHOLD;
    unsigned long bestLength;
    unsigned long bestOffset;
    unsigned long curPos;
    unsigned char firstByte;
    unsigned long hash;
    unsigned long matchPos;
    unsigned long matchLength;
    unsigned long lengthCode;
    unsigned long distCode;
    unsigned long totalFreq;
    unsigned long bitBuffer;
    unsigned long bitCount;
    unsigned long extraBits;
    unsigned long extraBitValue;
    struct lh_code_lookup *codeLookup = NULL;
    struct lh_code_lookup *distLookup = NULL;
    long result = LH_OK;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough - worst case is input size + 5% + 512 bytes */
    if (*outSize < inSize + (inSize / 8) + 64) {
        return LH_ERR_NO_MEMORY;
    }
    
    window = (unsigned char *)malloc(LH_RING_BUFFER_SIZE);
    if (!window) {
        return LH_ERR_NO_MEMORY;
    }
    hashLinks = (unsigned long *)malloc(LH_RING_BUFFER_SIZE * sizeof(unsigned long));
    if (!hashLinks) {
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    codeLookup = (struct lh_code_lookup *)malloc(NUM_CODES * sizeof(struct lh_code_lookup));
    if (!codeLookup) {
        free(hashLinks);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    distLookup = (struct lh_code_lookup *)malloc(DISTANCE_TABLE_SIZE * sizeof(struct lh_code_lookup));
    if (!distLookup) {
        free(codeLookup);
        free(hashLinks);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize window with spaces */
    memset(window, ' ', LH_RING_BUFFER_SIZE);
    
    /* Initialize hash table and links */
    for (i = 0; i < 256; i++) {
        hashTable[i] = LH_RING_BUFFER_SIZE; /* Empty marker */
    }
    
    for (i = 0; i < LH_RING_BUFFER_SIZE; i++) {
        hashLinks[i] = LH_RING_BUFFER_SIZE; /* Empty marker */
    }
    
    /* First pass: Build frequency tables for literals and lengths */
    memset(codeLookup, 0, NUM_CODES * sizeof(struct lh_code_lookup));
    memset(distLookup, 0, DISTANCE_TABLE_SIZE * sizeof(struct lh_code_lookup));
    
    windowPos = 0;
    
    /* Insert first bytes into window */
    for (i = 0; i < lookahead; i++) {
        window[windowPos++] = input[i];
        if (windowPos >= LH_RING_BUFFER_SIZE) {
            windowPos = 0;
        }
    }
    
    startPos = lookahead;
    
    /* Process input data to collect frequency statistics */
    while (startPos < inSize) {
        bestLength = 0;
        bestOffset = 0;
        curPos = startPos - lookahead;
        firstByte = input[curPos];
        distCode = 0;
        
        /* Find best match using hash table */
        hash = firstByte;
        matchPos = hashTable[hash];
        
        /* Find longest match */
        while (matchPos < LH_RING_BUFFER_SIZE) {
            /* Check if we have a match */
            matchLength = 0;
            while (matchLength < lookahead && 
                   curPos + matchLength < inSize && 
                   window[(matchPos + matchLength) % LH_RING_BUFFER_SIZE] == input[curPos + matchLength]) {
                matchLength++;
            }
            
            /* If we found a match that's long enough and better than current best */
            if (matchLength >= MATCH_LENGTH_THRESHOLD && matchLength > bestLength) {
                bestLength = matchLength;
                bestOffset = (windowPos - matchPos) & (LH_RING_BUFFER_SIZE - 1);
                
                /* If we found a very good match, stop searching */
                if (matchLength >= 32) {
                    break;
                }
            }
            
            /* Try next match in chain */
            matchPos = hashLinks[matchPos];
        }
        
        /* Update frequency tables based on match/literal decision */
        if (bestLength >= MATCH_LENGTH_THRESHOLD) {
            /* We found a match - encode as length and distance */
            lengthCode = bestLength - MATCH_LENGTH_THRESHOLD;
            if (lengthCode >= NUM_LEN_CODES) {
                lengthCode = NUM_LEN_CODES - 1;
            }
            
            codeLookup[NUM_LITERAL_CODES + lengthCode].freq++;
            
            /* Find appropriate distance code */
            if (bestOffset < 8) {
                distCode = bestOffset;
            } else if (bestOffset < 24) {
                distCode = 8 + ((bestOffset - 8) >> 1);
            } else if (bestOffset < 88) {
                distCode = 16 + ((bestOffset - 24) >> 2);
            } else {
                distCode = 32 + ((bestOffset - 88) >> 3);
            }
            
            distLookup[distCode].freq++;
            
            /* Advance by match length */
            for (i = 0; i < bestLength; i++) {
                if (startPos < inSize) {
                    /* Insert into window */
                    window[windowPos] = input[startPos];
                    
                    /* Update hash chain */
                    hash = input[startPos];
                    hashLinks[windowPos] = hashTable[hash];
                    hashTable[hash] = windowPos;
                    
                    /* Move window position */
                    windowPos = (windowPos + 1) & (LH_RING_BUFFER_SIZE - 1);
                    
                    startPos++;
                    lookahead--;
                }
            }
        } else {
            /* No good match found - encode as literal */
            codeLookup[firstByte].freq++;
            
            /* Insert into window */
            window[windowPos] = firstByte;
            
            /* Update hash chain */
            hashLinks[windowPos] = hashTable[hash];
            hashTable[hash] = windowPos;
            
            /* Move window position */
            windowPos = (windowPos + 1) & (LH_RING_BUFFER_SIZE - 1);
            
            startPos++;
            lookahead--;
        }
        
        /* Refill lookahead buffer */
        while (lookahead < LH_MAX_MATCH_LENGTH - 1 && startPos < inSize) {
            lookahead++;
            startPos++;
        }
    }
    
    /* Build Huffman codes for literals and lengths */
    /* This is a simplified version - a real implementation would build optimal codes */
    totalFreq = 0;
    for (i = 0; i < NUM_CODES; i++) {
        if (codeLookup[i].freq > 0) {
            totalFreq += codeLookup[i].freq;
        }
    }
    
    /* Ensure every symbol has at least a minimal frequency */
    for (i = 0; i < NUM_CODES; i++) {
        if (codeLookup[i].freq == 0) {
            codeLookup[i].freq = 1;
        }
    }
    if (totalFreq == 0) {
        totalFreq = 1;
    }
    
    /* Simple length calculation based on frequencies */
    for (i = 0; i < NUM_CODES; i++) {
        codeLookup[i].codeLen = 15 - (unsigned char)((codeLookup[i].freq * 10) / totalFreq);
        if (codeLookup[i].codeLen < 1) codeLookup[i].codeLen = 1;
        if (codeLookup[i].codeLen > 15) codeLookup[i].codeLen = 15;
    }
    
    /* Build distance codes similarly */
    totalFreq = 0;
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        if (distLookup[i].freq > 0) {
            totalFreq += distLookup[i].freq;
        }
    }
    
    /* Ensure every distance has at least a minimal frequency */
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        if (distLookup[i].freq == 0) {
            distLookup[i].freq = 1;
        }
    }
    if (totalFreq == 0) {
        totalFreq = 1;
    }
    
    /* Simple length calculation for distances */
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        distLookup[i].codeLen = 7 - (unsigned char)((distLookup[i].freq * 4) / totalFreq);
        if (distLookup[i].codeLen < 1) distLookup[i].codeLen = 1;
        if (distLookup[i].codeLen > 7) distLookup[i].codeLen = 7;
    }
    
    /* Write code length tables to output */
    
    /* Code lengths for literals+lengths (4 bits each) */
    for (i = 0; i < NUM_CODES; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = codeLookup[i].codeLen;
    }
    
    /* Code lengths for distances (3 bits each) */
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = distLookup[i].codeLen;
    }
    
    /* Reset window and hash table for second pass */
    memset(window, ' ', LH_RING_BUFFER_SIZE);
    for (i = 0; i < 256; i++) {
        hashTable[i] = LH_RING_BUFFER_SIZE;
    }
    for (i = 0; i < LH_RING_BUFFER_SIZE; i++) {
        hashLinks[i] = LH_RING_BUFFER_SIZE;
    }
    
    /* Initialize bitstream for output */
    bitBuffer = 0;
    bitCount = 0;
    
    /* Second pass: Compress data using collected statistics */
    windowPos = 0;
    startPos = 0;
    lookahead = (inSize < LH_MAX_MATCH_LENGTH) ? inSize : LH_MAX_MATCH_LENGTH;
    
    /* Insert first bytes into window */
    for (i = 0; i < lookahead; i++) {
        window[windowPos++] = input[i];
        if (windowPos >= LH_RING_BUFFER_SIZE) {
            windowPos = 0;
        }
    }
    
    startPos = lookahead;
    
    /* Process input data for compression */
    while (startPos - lookahead < inSize) {
        bestLength = 0;
        bestOffset = 0;
        curPos = startPos - lookahead;
        firstByte = input[curPos];
        distCode = 0;
        extraBits = 0;
        extraBitValue = 0;
        
        /* Find best match using hash table */
        hash = firstByte;
        matchPos = hashTable[hash];
        
        /* Find longest match */
        while (matchPos < LH_RING_BUFFER_SIZE) {
            /* Check if we have a match */
            matchLength = 0;
            while (matchLength < lookahead && 
                   curPos + matchLength < inSize && 
                   window[(matchPos + matchLength) % LH_RING_BUFFER_SIZE] == input[curPos + matchLength]) {
                matchLength++;
            }
            
            /* If we found a match that's long enough and better than current best */
            if (matchLength >= MATCH_LENGTH_THRESHOLD && matchLength > bestLength) {
                bestLength = matchLength;
                bestOffset = (windowPos - matchPos) & (LH_RING_BUFFER_SIZE - 1);
                
                /* If we found a very good match, stop searching */
                if (matchLength >= 32) {
                    break;
                }
            }
            
            /* Try next match in chain */
            matchPos = hashLinks[matchPos];
        }
        
        /* Encode match or literal */
        if (bestLength >= MATCH_LENGTH_THRESHOLD) {
            /* We found a match - encode as length and distance */
            lengthCode = bestLength - MATCH_LENGTH_THRESHOLD;
            if (lengthCode >= NUM_LEN_CODES) {
                lengthCode = NUM_LEN_CODES - 1;
            }
            
            /* Encode length code */
            if (lh_encode_write_bits(NUM_LITERAL_CODES + lengthCode,
                codeLookup[NUM_LITERAL_CODES + lengthCode].codeLen,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
            
            /* Find appropriate distance code */
            if (bestOffset < 8) {
                distCode = bestOffset;
                extraBits = 0;
            } else if (bestOffset < 24) {
                distCode = 8 + ((bestOffset - 8) >> 1);
                extraBits = 1;
                extraBitValue = (bestOffset - 8) & 1;
            } else if (bestOffset < 88) {
                distCode = 16 + ((bestOffset - 24) >> 2);
                extraBits = 2;
                extraBitValue = (bestOffset - 24) & 3;
            } else {
                distCode = 32 + ((bestOffset - 88) >> 3);
                extraBits = 3;
                extraBitValue = (bestOffset - 88) & 7;
            }
            
            /* Encode distance code */
            if (lh_encode_write_bits(distCode, distLookup[distCode].codeLen,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
            
            /* Encode extra bits for distance if needed */
            if (extraBits > 0) {
                if (lh_encode_write_bits(extraBitValue, extraBits,
                    &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                    result = LH_ERR_NO_MEMORY;
                    goto cleanup;
                }
            }
            
            /* Advance by match length */
            for (i = 0; i < bestLength; i++) {
                if (startPos < inSize) {
                    /* Insert into window */
                    window[windowPos] = input[startPos];
                    
                    /* Update hash chain */
                    hash = input[startPos];
                    hashLinks[windowPos] = hashTable[hash];
                    hashTable[hash] = windowPos;
                    
                    /* Move window position */
                    windowPos = (windowPos + 1) & (LH_RING_BUFFER_SIZE - 1);
                    
                    startPos++;
                    lookahead--;
                }
            }
        } else {
            /* No good match found - encode as literal */
            if (lh_encode_write_bits(firstByte, codeLookup[firstByte].codeLen,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
            
            /* Insert into window */
            window[windowPos] = firstByte;
            
            /* Update hash chain */
            hashLinks[windowPos] = hashTable[hash];
            hashTable[hash] = windowPos;
            
            /* Move window position */
            windowPos = (windowPos + 1) & (LH_RING_BUFFER_SIZE - 1);
            
            startPos++;
            lookahead--;
        }
        
        /* Refill lookahead buffer */
        while (lookahead < LH_MAX_MATCH_LENGTH - 1 && startPos < inSize) {
            lookahead++;
            startPos++;
        }
    }
    
    /* Flush any remaining bits */
    if (bitCount > 0) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = bitBuffer & 0xFF;
    }
    
    /* Update output size */
    *outSize = outPos;
    
cleanup:
    if (distLookup) {
        free(distLookup);
    }
    if (codeLookup) {
        free(codeLookup);
    }
    if (hashLinks) {
        free(hashLinks);
    }
    if (window) {
        free(window);
    }
    return result;
}

/* lh_decompress_lh6 is implemented in lh_lh5dec.c (real LH4..LH7 decoder). */

long lh_compress_lh6(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long i;
    unsigned char *window = NULL;
    unsigned long windowPos = 0;
    unsigned long *hashTable = NULL;
    unsigned long *hashLinks = NULL;
    const unsigned long NUM_LITERAL_CODES = LH6_LITERAL_CODES;
    const unsigned long NUM_LEN_CODES = LH6_LENGTH_CODES;
    const unsigned long NUM_CODES = LH6_TOTAL_CODES;
    const unsigned long DISTANCE_TABLE_SIZE = 128;
    unsigned long startPos = 0;
    unsigned long lookahead = (inSize < MATCH_LENGTH_THRESHOLD) ? inSize : MATCH_LENGTH_THRESHOLD;
    long result = LH_OK;
    unsigned long totalFreq = 0;
    unsigned long usePresetDict = 1; /* Use predefined table */
    unsigned long distanceCode = 0;
    unsigned long extraBits = 0;
    unsigned long lengthRemaining = 0;
    
    struct lh_code_lookup *codeLookup = NULL;
    struct lh_code_lookup *distLookup = NULL;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough - worst case is input size + 5% + 512 bytes */
    if (*outSize < inSize + (inSize / 8) + 64) {
        return LH_ERR_NO_MEMORY;
    }
    
    /* Allocate memory for window and hash tables */
    window = malloc(LH6_DICT_SIZE);
    if (!window) {
        return LH_ERR_NO_MEMORY;
    }
    
    hashTable = malloc(256 * sizeof(unsigned long));
    if (!hashTable) {
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    hashLinks = malloc(LH6_DICT_SIZE * sizeof(unsigned long));
    if (!hashLinks) {
        free(hashTable);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    codeLookup = malloc(NUM_CODES * sizeof(struct lh_code_lookup));
    if (!codeLookup) {
        free(hashLinks);
        free(hashTable);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    distLookup = malloc(DISTANCE_TABLE_SIZE * sizeof(struct lh_code_lookup));
    if (!distLookup) {
        free(codeLookup);
        free(hashLinks);
        free(hashTable);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize window with spaces */
    memset(window, ' ', LH6_DICT_SIZE);
    
    /* Initialize hash table and links */
    for (i = 0; i < 256; i++) {
        hashTable[i] = LH6_DICT_SIZE; /* Empty marker */
    }
    
    for (i = 0; i < LH6_DICT_SIZE; i++) {
        hashLinks[i] = LH6_DICT_SIZE; /* Empty marker */
    }
    
    /* First pass: Build frequency tables for literals and lengths */
    memset(codeLookup, 0, NUM_CODES * sizeof(struct lh_code_lookup));
    memset(distLookup, 0, DISTANCE_TABLE_SIZE * sizeof(struct lh_code_lookup));
    
    windowPos = 0;
    
    for (i = 0; i < lookahead; i++) {
        window[windowPos++] = input[i];
        if (windowPos >= LH6_DICT_SIZE) {
            windowPos = 0;
        }
    }
    
    startPos = lookahead;
    
    /* Process input data to collect frequency statistics */
    while (startPos < inSize) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        unsigned long curPos = startPos - lookahead;
        unsigned char firstByte = input[curPos];
        unsigned long distCode = 0;
        /* Find best match using hash table */
        unsigned long hash = firstByte;
        unsigned long matchPos = hashTable[hash];
        
        /* Find longest match */
        while (matchPos < LH6_DICT_SIZE) {
            /* Check if we have a match */
            unsigned long matchLength = 0;
            while (matchLength < lookahead && 
                   curPos + matchLength < inSize && 
                   window[(matchPos + matchLength) % LH6_DICT_SIZE] == input[curPos + matchLength]) {
                matchLength++;
            }
            
            /* If we found a match that's long enough and better than current best */
            if (matchLength >= MATCH_LENGTH_THRESHOLD && matchLength > bestLength) {
                bestLength = matchLength;
                bestOffset = (windowPos - matchPos) & (LH6_DICT_SIZE - 1);
                
                /* If we found a very good match, stop searching */
                if (matchLength >= 64) {
                    break;
                }
            }
            
            /* Try next match in chain */
            matchPos = hashLinks[matchPos];
        }
        
        /* Update frequency tables based on match/literal decision */
        if (bestLength >= MATCH_LENGTH_THRESHOLD) {
            /* We found a match - encode as length and distance */
            unsigned long lengthCode = bestLength - MATCH_LENGTH_THRESHOLD;
            if (lengthCode >= NUM_LEN_CODES) {
                lengthCode = NUM_LEN_CODES - 1;
            }
            
            codeLookup[NUM_LITERAL_CODES + lengthCode].freq++;
            
            /* Find appropriate distance code */
            
            if (bestOffset < 8) {
                distCode = bestOffset;
            } else if (bestOffset < 24) {
                distCode = 8 + ((bestOffset - 8) >> 1);
            } else if (bestOffset < 88) {
                distCode = 16 + ((bestOffset - 24) >> 2);
            } else if (bestOffset < 344) {
                distCode = 32 + ((bestOffset - 88) >> 3);
            } else {
                distCode = 64 + ((bestOffset - 344) >> 4);
            }
            
            distLookup[distCode].freq++;
            
            /* Advance by match length */
            for (i = 0; i < bestLength; i++) {
                if (startPos < inSize) {
                    /* Insert into window */
                    window[windowPos] = input[startPos];
                    
                    /* Update hash chain */
                    hash = input[startPos];
                    hashLinks[windowPos] = hashTable[hash];
                    hashTable[hash] = windowPos;
                    
                    /* Move window position */
                    windowPos = (windowPos + 1) & (LH6_DICT_SIZE - 1);
                    
                    startPos++;
                    lookahead--;
                }
            }
        } else {
            /* No good match found - encode as literal */
            codeLookup[firstByte].freq++;
            
            /* Insert into window */
            window[windowPos] = firstByte;
            
            /* Update hash chain */
            hashLinks[windowPos] = hashTable[hash];
            hashTable[hash] = windowPos;
            
            /* Move window position */
            windowPos = (windowPos + 1) & (LH6_DICT_SIZE - 1);
            
            startPos++;
            lookahead--;
        }
        
        /* Refill lookahead buffer */
        while (lookahead < LH6_MAX_MATCH - 1 && startPos < inSize) {
            lookahead++;
            startPos++;
        }
    }
    
    /* Build Huffman codes for literals and lengths */
    /* This is a simplified version - a real implementation would build optimal codes */

    for (i = 0; i < NUM_CODES; i++) {
        if (codeLookup[i].freq > 0) {
            totalFreq += codeLookup[i].freq;
        }
    }
    
    /* Ensure every symbol has at least a minimal frequency */
    for (i = 0; i < NUM_CODES; i++) {
        if (codeLookup[i].freq == 0) {
            codeLookup[i].freq = 1;
        }
    }
    if (totalFreq == 0) {
        totalFreq = 1;
    }
    
    /* Simple length calculation based on frequencies */
    for (i = 0; i < NUM_CODES; i++) {
        codeLookup[i].codeLen = 15 - (unsigned char)((codeLookup[i].freq * 10) / totalFreq);
        if (codeLookup[i].codeLen < 1) codeLookup[i].codeLen = 1;
        if (codeLookup[i].codeLen > 15) codeLookup[i].codeLen = 15;
    }
    
    /* Build distance codes similarly */
    totalFreq = 0;
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        if (distLookup[i].freq > 0) {
            totalFreq += distLookup[i].freq;
        }
    }
    
    /* Ensure every distance has at least a minimal frequency */
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        if (distLookup[i].freq == 0) {
            distLookup[i].freq = 1;
        }
    }
    if (totalFreq == 0) {
        totalFreq = 1;
    }
    
    /* Simple length calculation for distances */
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        distLookup[i].codeLen = 8 - (unsigned char)((distLookup[i].freq * 4) / totalFreq);
        if (distLookup[i].codeLen < 1) distLookup[i].codeLen = 1;
        if (distLookup[i].codeLen > 8) distLookup[i].codeLen = 8;
    }
    
    /* Write code length tables to output */
    
    /* Code lengths for literals+lengths (4 bits each) */
    for (i = 0; i < NUM_CODES; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = codeLookup[i].codeLen;
    }
    
    /* Code lengths for distances (4 bits each for LH6) */
    for (i = 0; i < DISTANCE_TABLE_SIZE; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = distLookup[i].codeLen;
    }
    
    /* Reset window and hash table for second pass */
    memset(window, ' ', LH6_DICT_SIZE);
    for (i = 0; i < 256; i++) {
        hashTable[i] = LH6_DICT_SIZE;
    }
    for (i = 0; i < LH6_DICT_SIZE; i++) {
        hashLinks[i] = LH6_DICT_SIZE;
    }
    
    /* Initialize bitstream for output (uses file-scope bitBuffer/bitCount) */
    bitBuffer = 0;
    bitCount = 0;
    
    /* Second pass: Compress data using collected statistics */
    windowPos = 0;
    startPos = 0;
    lookahead = (inSize < LH6_MAX_MATCH) ? inSize : LH6_MAX_MATCH;
    
    /* Insert first bytes into window */
    for (i = 0; i < lookahead; i++) {
        window[windowPos++] = input[i];
        if (windowPos >= LH6_DICT_SIZE) {
            windowPos = 0;
        }
    }
    
    startPos = lookahead;
    
    /* Process input data for compression */
    while (startPos - lookahead < inSize) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        unsigned long curPos = startPos - lookahead;
        unsigned char firstByte = input[curPos];
        unsigned long hash = firstByte;
        unsigned long matchPos = hashTable[hash];
        unsigned long distCode = 0;
        
        /* Find best match using hash table */
        while (matchPos < LH6_DICT_SIZE) {
            /* Check if we have a match */
            unsigned long matchLength = 0;
            while (matchLength < lookahead && 
                   curPos + matchLength < inSize && 
                   window[(matchPos + matchLength) % LH6_DICT_SIZE] == input[curPos + matchLength]) {
                matchLength++;
            }
            
            /* If we found a match that's long enough and better than current best */
            if (matchLength >= MATCH_LENGTH_THRESHOLD && matchLength > bestLength) {
                bestLength = matchLength;
                bestOffset = (windowPos - matchPos) & (LH6_DICT_SIZE - 1);
                
                /* If we found a very good match, stop searching */
                if (matchLength >= 64) {
                    break;
                }
            }
            
            /* Try next match in chain */
            matchPos = hashLinks[matchPos];
        }
        
        /* Update frequency tables based on match/literal decision */
        if (bestLength >= MATCH_LENGTH_THRESHOLD) {
            /* We found a match - encode as length and distance */
            unsigned long lengthCode = bestLength - MATCH_LENGTH_THRESHOLD;
            if (lengthCode >= NUM_LEN_CODES) {
                lengthCode = NUM_LEN_CODES - 1;
            }
            
            codeLookup[NUM_LITERAL_CODES + lengthCode].freq++;
            
            /* Find appropriate distance code */
            if (bestOffset < 8) {
                distCode = bestOffset;
            } else if (bestOffset < 24) {
                distCode = 8 + ((bestOffset - 8) >> 1);
            } else if (bestOffset < 88) {
                distCode = 16 + ((bestOffset - 24) >> 2);
            } else if (bestOffset < 344) {
                distCode = 32 + ((bestOffset - 88) >> 3);
            } else {
                distCode = 64 + ((bestOffset - 344) >> 4);
            }
            
            distLookup[distCode].freq++;
            
            /* Advance by match length */
            for (i = 0; i < bestLength; i++) {
                if (startPos < inSize) {
                    /* Insert into window */
                    window[windowPos] = input[startPos];
                    
                    /* Update hash chain */
                    hash = input[startPos];
                    hashLinks[windowPos] = hashTable[hash];
                    hashTable[hash] = windowPos;
                    
                    /* Move window position */
                    windowPos = (windowPos + 1) & (LH6_DICT_SIZE - 1);
                    
                    startPos++;
                    lookahead--;
                }
            }
        } else {
            /* No good match found - encode as literal */
            codeLookup[firstByte].freq++;
            
            /* Insert into window */
            window[windowPos] = firstByte;
            
            /* Update hash chain */
            hashLinks[windowPos] = hashTable[hash];
            hashTable[hash] = windowPos;
            
            /* Move window position */
            windowPos = (windowPos + 1) & (LH6_DICT_SIZE - 1);
            
            startPos++;
            lookahead--;
        }
        
        /* Refill lookahead buffer */
        while (lookahead < LH6_MAX_MATCH - 1 && startPos < inSize) {
            lookahead++;
            startPos++;
        }
    }
    
    /* Flush any remaining bits */
    if (bitCount > 0) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = bitBuffer & 0xFF;
    }
    
    /* Update output size */
    *outSize = outPos;
    
cleanup:
    /* Free all allocated resources */
    if (distLookup) free(distLookup);
    if (codeLookup) free(codeLookup);
    if (hashLinks) free(hashLinks);
    if (hashTable) free(hashTable);
    if (window) free(window);
    
    return result;
}

/* LH7 decompression - advanced LZSS with adaptive Huffman coding */
/* lh_decompress_lh7 is implemented in lh_lh5dec.c (real LH4..LH7 decoder). */

long lh_compress_lh7(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    /* Declare all variables at the start */
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long i, j;
    unsigned char *window = NULL;
    unsigned long windowPos = 0;
    unsigned long *hashTable = NULL;
    unsigned long *hashLinks = NULL;
    const unsigned long NUM_LITERAL_CODES = LH7_LITERAL_CODES;
    const unsigned long NUM_LEN_CODES = LH7_LENGTH_CODES;
    const unsigned long NUM_CODES = LH7_TOTAL_CODES;
    const unsigned long OFFSET_BITS = LH7_OFFSET_BITS;
    long result = LH_OK;
    unsigned long usePresetDict = 1;
    unsigned long distanceCode = 0;
    unsigned long extraBits = 0;
    unsigned long lengthRemaining = 0;
    unsigned long startPos = 0;
    unsigned long lookahead = (inSize < MATCH_LENGTH_THRESHOLD) ? inSize : MATCH_LENGTH_THRESHOLD;
    
    /* Length tables matching the decompression */
    unsigned long lengthBase[LH7_LENGTH_CODES];
    unsigned long lengthExtraBits[LH7_LENGTH_CODES];
    
    struct lh_code_lookup *codeLookup = NULL;
    struct lh_code_lookup *distCodeLookup = NULL;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough - worst case is input size + 5% + 512 bytes */
    if (*outSize < inSize + (inSize / 8) + 64) {
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize length tables */
    for (i = 0; i < LH7_LENGTH_CODES; i++) {
        if (i < 8) {
            lengthBase[i] = i + MATCH_LENGTH_THRESHOLD;
            lengthExtraBits[i] = 0;
        } else if (i < 16) {
            lengthBase[i] = 8 + MATCH_LENGTH_THRESHOLD + (i - 8) * 2;
            lengthExtraBits[i] = 1;
        } else if (i < 24) {
            lengthBase[i] = 24 + MATCH_LENGTH_THRESHOLD + (i - 16) * 4;
            lengthExtraBits[i] = 2;
        } else {
            lengthBase[i] = 56 + MATCH_LENGTH_THRESHOLD + (i - 24) * 8;
            lengthExtraBits[i] = 3;
        }
    }
    
    /* Allocate memory for window and hash tables */
    window = malloc(LH7_DICT_SIZE);
    if (!window) {
        return LH_ERR_NO_MEMORY;
    }
    
    hashTable = malloc(256 * sizeof(unsigned long));
    if (!hashTable) {
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    hashLinks = malloc(LH7_DICT_SIZE * sizeof(unsigned long));
    if (!hashLinks) {
        free(hashTable);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    codeLookup = malloc(NUM_CODES * sizeof(struct lh_code_lookup));
    if (!codeLookup) {
        free(hashLinks);
        free(hashTable);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    distCodeLookup = malloc(OFFSET_BITS * sizeof(struct lh_code_lookup));
    if (!distCodeLookup) {
        free(codeLookup);
        free(hashLinks);
        free(hashTable);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize window with spaces */
    memset(window, ' ', LH7_DICT_SIZE);
    
    /* Initialize hash table and links */
    for (i = 0; i < 256; i++) {
        hashTable[i] = LH7_DICT_SIZE; /* Empty marker */
    }
    
    for (i = 0; i < LH7_DICT_SIZE; i++) {
        hashLinks[i] = LH7_DICT_SIZE; /* Empty marker */
    }
    
    /* First pass: Build frequency tables for literals and lengths */
    memset(codeLookup, 0, NUM_CODES * sizeof(struct lh_code_lookup));
    memset(distCodeLookup, 0, OFFSET_BITS * sizeof(struct lh_code_lookup));
    
    windowPos = 0;
    
    for (i = 0; i < lookahead; i++) {
        window[windowPos++] = input[i];
        if (windowPos >= LH7_DICT_SIZE) {
            windowPos = 0;
        }
    }
    
    startPos = lookahead;
    
    /* Process input data to collect frequency statistics */
    while (startPos < inSize) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        unsigned long curPos = startPos - lookahead;
        unsigned char firstByte = input[curPos];
        
        /* Find best match using hash table */
        unsigned long hash = firstByte;
        unsigned long matchPos = hashTable[hash];
        
        /* Find longest match */
        while (matchPos < LH7_DICT_SIZE) {
            /* Check if we have a match */
            unsigned long matchLength = 0;
            while (matchLength < lookahead && 
                   curPos + matchLength < inSize && 
                   window[(matchPos + matchLength) % LH7_DICT_SIZE] == input[curPos + matchLength]) {
                matchLength++;
            }
            
            /* If we found a match that's long enough and better than current best */
            if (matchLength >= MATCH_LENGTH_THRESHOLD && matchLength > bestLength) {
                bestLength = matchLength;
                bestOffset = (windowPos - matchPos) & (LH7_DICT_SIZE - 1);
                
                /* If we found a very good match, stop searching */
                if (matchLength >= 64) {
                    break;
                }
            }
            
            /* Try next match in chain */
            matchPos = hashLinks[matchPos];
        }
        
        /* Update frequency tables based on match/literal decision */
        if (bestLength >= MATCH_LENGTH_THRESHOLD) {
            /* We found a match - determine length code */
            unsigned long lengthCode = 0;
            unsigned long lengthRemaining = bestLength - MATCH_LENGTH_THRESHOLD;
            
            /* Find the appropriate length code */
            for (i = 0; i < NUM_LEN_CODES && lengthBase[i] <= bestLength; i++) {
                if (i == NUM_LEN_CODES - 1 || lengthBase[i+1] > bestLength) {
                    lengthCode = i;
                    break;
                }
            }
            
            codeLookup[NUM_LITERAL_CODES + lengthCode].freq++;
            
            /* Update offset bit frequencies */
            for (i = 0; i < OFFSET_BITS; i++) {
                distCodeLookup[i].freq += (bestOffset >> i) & 1;
            }
            
            /* Advance by match length */
            for (i = 0; i < bestLength; i++) {
                if (startPos < inSize) {
                    /* Insert into window */
                    window[windowPos] = input[startPos];
                    
                    /* Update hash chain */
                    hash = input[startPos];
                    hashLinks[windowPos] = hashTable[hash];
                    hashTable[hash] = windowPos;
                    
                    /* Move window position */
                    windowPos = (windowPos + 1) & (LH7_DICT_SIZE - 1);
                    
                    startPos++;
                    lookahead--;
                }
            }
        } else {
            /* No good match found - encode as literal */
            codeLookup[firstByte].freq++;
            
            /* Insert into window */
            window[windowPos] = firstByte;
            
            /* Update hash chain */
            hashLinks[windowPos] = hashTable[hash];
            hashTable[hash] = windowPos;
            
            /* Move window position */
            windowPos = (windowPos + 1) & (LH7_DICT_SIZE - 1);
            
            startPos++;
            lookahead--;
        }
        
        /* Refill lookahead buffer */
        while (lookahead < LH7_MAX_MATCH - 1 && startPos < inSize) {
            lookahead++;
            startPos++;
        }
    }
    
    /* Build Huffman codes for literals and lengths */
    /* We'll use a predefined code table for LH7 */
    if (usePresetDict) {
        /* Set up fixed code lengths */
        for (i = 0; i < NUM_CODES; i++) {
            if (i < NUM_LITERAL_CODES) {
                /* Literals have fixed length of 9 bits */
                codeLookup[i].codeLen = 9;
            } else {
                /* Length codes have fixed length of 6 bits */
                codeLookup[i].codeLen = 6;
            }
        }
        
        for (i = 0; i < OFFSET_BITS; i++) {
            /* Offset bits have fixed length of 5 bits */
            distCodeLookup[i].codeLen = 5;
        }
    } else {
        /* Build optimal Huffman codes based on frequencies */
        unsigned long totalFreq = 0;
        for (i = 0; i < NUM_CODES; i++) {
            if (codeLookup[i].freq > 0) {
                totalFreq += codeLookup[i].freq;
            }
        }
        
        /* Ensure every symbol has at least a minimal frequency */
        for (i = 0; i < NUM_CODES; i++) {
            if (codeLookup[i].freq == 0) {
                codeLookup[i].freq = 1;
            }
        }
        if (totalFreq == 0) {
            totalFreq = 1;
        }
        
        /* Calculate code lengths */
        for (i = 0; i < NUM_CODES; i++) {
            codeLookup[i].codeLen = 15 - (unsigned char)((codeLookup[i].freq * 10) / totalFreq);
            if (codeLookup[i].codeLen < 1) codeLookup[i].codeLen = 1;
            if (codeLookup[i].codeLen > 15) codeLookup[i].codeLen = 15;
        }
        
        /* Similar for distance codes */
        totalFreq = 0;
        for (i = 0; i < OFFSET_BITS; i++) {
            if (distCodeLookup[i].freq > 0) {
                totalFreq += distCodeLookup[i].freq;
            }
        }
        
        for (i = 0; i < OFFSET_BITS; i++) {
            if (distCodeLookup[i].freq == 0) {
                distCodeLookup[i].freq = 1;
            }
        }
        if (totalFreq == 0) {
            totalFreq = 1;
        }
        
        for (i = 0; i < OFFSET_BITS; i++) {
            distCodeLookup[i].codeLen = 7 - (unsigned char)((distCodeLookup[i].freq * 4) / totalFreq);
            if (distCodeLookup[i].codeLen < 1) distCodeLookup[i].codeLen = 1;
            if (distCodeLookup[i].codeLen > 7) distCodeLookup[i].codeLen = 7;
        }
    }
    
    /* Initialize bitstream for output */
    bitBuffer = 0;
    bitCount = 0;
    
    /* Write flag for preset dictionary */
    if (lh_encode_write_bits(usePresetDict, 1,
        &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
        result = LH_ERR_NO_MEMORY;
        goto cleanup;
    }
    
    if (!usePresetDict) {
        /* Write code length bits */
        unsigned long codeLengthBits = 7; /* 7 bits per code length (value: 5-12) */
        unsigned long distLengthBits = 5; /* 5 bits per distance length (value: 4-7) */
        if (lh_encode_write_bits(codeLengthBits - 5, 3,
            &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        
        /* Write code lengths for main tree */
        for (i = 0; i < NUM_CODES; i++) {
            if (lh_encode_write_bits(codeLookup[i].codeLen, codeLengthBits,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
        }
        
        /* Write distance code length bits */
        if (lh_encode_write_bits(distLengthBits - 4, 2,
            &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        
        /* Write code lengths for distance tree */
        for (i = 0; i < OFFSET_BITS; i++) {
            if (lh_encode_write_bits(distCodeLookup[i].codeLen, distLengthBits,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
        }
    }
    
    /* Reset window and hash table for second pass */
    memset(window, ' ', LH7_DICT_SIZE);
    for (i = 0; i < 256; i++) {
        hashTable[i] = LH7_DICT_SIZE;
    }
    for (i = 0; i < LH7_DICT_SIZE; i++) {
        hashLinks[i] = LH7_DICT_SIZE;
    }
    
    /* Second pass: Compress data */
    windowPos = 0;
    startPos = 0;
    lookahead = (inSize < LH7_MAX_MATCH) ? inSize : LH7_MAX_MATCH;
    
    /* Insert first bytes into window */
    for (i = 0; i < lookahead; i++) {
        window[windowPos++] = input[i];
        if (windowPos >= LH7_DICT_SIZE) {
            windowPos = 0;
        }
    }
    
    startPos = lookahead;
    
    /* Process input data for compression */
    while (startPos - lookahead < inSize) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        unsigned long curPos = startPos - lookahead;
        unsigned char firstByte = input[curPos];
        
        /* Find best match using hash table */
        unsigned long hash = firstByte;
        unsigned long matchPos = hashTable[hash];
        
        /* Find longest match */
        while (matchPos < LH7_DICT_SIZE) {
            /* Check if we have a match */
            unsigned long matchLength = 0;
            while (matchLength < lookahead && 
                   curPos + matchLength < inSize && 
                   window[(matchPos + matchLength) % LH7_DICT_SIZE] == input[curPos + matchLength]) {
                matchLength++;
            }
            
            /* If we found a match that's long enough and better than current best */
            if (matchLength >= MATCH_LENGTH_THRESHOLD && matchLength > bestLength) {
                bestLength = matchLength;
                bestOffset = (windowPos - matchPos) & (LH7_DICT_SIZE - 1);
                
                /* If we found a very good match, stop searching */
                if (matchLength >= 64) {
                    break;
                }
            }
            
            /* Try next match in chain */
            matchPos = hashLinks[matchPos];
        }
        
        /* Encode match or literal */
        if (bestLength >= MATCH_LENGTH_THRESHOLD) {
            /* We found a match - determine length code */
            unsigned long lengthCode = 0;
            unsigned long lengthRemaining = bestLength - MATCH_LENGTH_THRESHOLD;
            
            /* Find the appropriate length code */
            for (i = 0; i < NUM_LEN_CODES && lengthBase[i] <= bestLength; i++) {
                if (i == NUM_LEN_CODES - 1 || lengthBase[i+1] > bestLength) {
                    lengthCode = i;
                    break;
                }
            }
            
            /* Encode length code */
            if (lh_encode_write_bits(NUM_LITERAL_CODES + lengthCode,
                codeLookup[NUM_LITERAL_CODES + lengthCode].codeLen,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
            
            /* Encode extra bits for length if needed */
            if (lengthExtraBits[lengthCode] > 0) {
                unsigned long extraBitValue = bestLength - lengthBase[lengthCode];
                if (lh_encode_write_bits(extraBitValue, lengthExtraBits[lengthCode],
                    &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                    result = LH_ERR_NO_MEMORY;
                    goto cleanup;
                }
            }
            
            /* Encode each bit of the offset */
            for (i = 0; i < OFFSET_BITS; i++) {
                unsigned long bit = (bestOffset >> i) & 1;
                if (lh_encode_write_bits(bit, 1,
                    &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                    result = LH_ERR_NO_MEMORY;
                    goto cleanup;
                }
            }
            
            /* Advance by match length */
            for (i = 0; i < bestLength; i++) {
                if (startPos < inSize) {
                    /* Insert into window */
                    window[windowPos] = input[startPos];
                    
                    /* Update hash chain */
                    hash = input[startPos];
                    hashLinks[windowPos] = hashTable[hash];
                    hashTable[hash] = windowPos;
                    
                    /* Move window position */
                    windowPos = (windowPos + 1) & (LH7_DICT_SIZE - 1);
                    
                    startPos++;
                    lookahead--;
                }
            }
        } else {
            /* No good match found - encode as literal */
            if (lh_encode_write_bits(firstByte, codeLookup[firstByte].codeLen,
                &bitBuffer, &bitCount, output, &outPos, *outSize) != LH_OK) {
                result = LH_ERR_NO_MEMORY;
                goto cleanup;
            }
            
            /* Insert into window */
            window[windowPos] = firstByte;
            
            /* Update hash chain */
            hashLinks[windowPos] = hashTable[hash];
            hashTable[hash] = windowPos;
            
            /* Move window position */
            windowPos = (windowPos + 1) & (LH7_DICT_SIZE - 1);
            
            startPos++;
            lookahead--;
        }
        
        /* Refill lookahead buffer */
        while (lookahead < LH7_MAX_MATCH - 1 && startPos < inSize) {
            lookahead++;
            startPos++;
        }
    }
    
    /* Flush any remaining bits */
    if (bitCount > 0) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = bitBuffer & 0xFF;
    }
    
    /* Update output size */
    *outSize = outPos;
    
cleanup:
    /* Free all allocated resources */
    if (distCodeLookup) free(distCodeLookup);
    if (codeLookup) free(codeLookup);
    if (hashLinks) free(hashLinks);
    if (hashTable) free(hashTable);
    if (window) free(window);
    
    return result;
}

/* LZS decompression - Sliding window compression method */
long lh_decompress_lzs(void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long inPos = 0;
    unsigned long outPos = 0;
    unsigned char window[LZS_WINDOW_SIZE];
    unsigned long windowPos = 0;
    unsigned long i, len, offset;
    unsigned char flags, mask;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || outSize == 0) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Clear window */
    memset(window, 0, LZS_WINDOW_SIZE);
    
    /* Main decompression loop */
    flags = 0;
    mask = 0;
    
    while (inPos < inSize && outPos < outSize) {
        /* Read new flags if needed */
        if (mask == 0) {
            if (inPos >= inSize) break;
            flags = input[inPos++];
            mask = 0x80;  /* Start with the highest bit */
        }
        
        if (flags & mask) {
            /* Match - read offset and length */
            if (inPos + 1 >= inSize) break;
            
            offset = input[inPos++];
            offset |= (input[inPos++] & 0xF0) << 4;
            len = (input[inPos - 1] & 0x0F) + LZS_BREAK_EVEN;
            
            /* Copy bytes from window to output */
            for (i = 0; i < len; i++) {
                unsigned char b = window[(offset + i) % LZS_WINDOW_SIZE];
                
                if (outPos >= outSize) {
                    return LH_ERR_INVALID_ARG; /* Output buffer overflow */
                }
                
                output[outPos++] = b;
                
                /* Add to window */
                window[windowPos] = b;
                windowPos = (windowPos + 1) % LZS_WINDOW_SIZE;
            }
        } else {
            /* Literal byte */
            if (inPos >= inSize) break;
            
            if (outPos >= outSize) {
                return LH_ERR_INVALID_ARG; /* Output buffer overflow */
            }
            
            output[outPos++] = input[inPos];
            
            /* Add to window */
            window[windowPos] = input[inPos++];
            windowPos = (windowPos + 1) % LZS_WINDOW_SIZE;
        }
        
        /* Move to next flag bit */
        mask >>= 1;
    }
    
    /* Return success if we decompressed the expected size */
    return (outPos == outSize) ? LH_OK : LH_ERR_INVALID_ARG;
}

long lh_compress_lzs(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long inPos = 0;
    unsigned long i, j;
    unsigned char window[LZS_WINDOW_SIZE];
    unsigned long windowPos = 0;
    unsigned char flagByte = 0;
    unsigned long flagPos = 0;
    unsigned long flagBit = 0x80;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough - worst case is input size + 12.5% + 12 bytes */
    if (*outSize < inSize + (inSize / 8) + 12) {
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize window with zeros */
    memset(window, 0, LZS_WINDOW_SIZE);
    
    /* Reserve space for the first flag byte */
    if (outPos >= *outSize) return LH_ERR_NO_MEMORY;
    flagPos = outPos++;
    output[flagPos] = 0;
    
    /* Process input data */
    while (inPos < inSize) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        
        /* Check if we need a new flag byte */
        if (flagBit == 0) {
            /* Start a new flag byte */
            if (outPos >= *outSize) return LH_ERR_NO_MEMORY;
            flagPos = outPos++;
            output[flagPos] = 0;
            flagBit = 0x80;
        }
        
        /* Look for the longest match in the window */
        if (inPos > LZS_BREAK_EVEN) {
            for (i = 0; i < LZS_WINDOW_SIZE; i++) {
                unsigned long matchLength = 0;
                
                /* Check how many bytes match */
                while (matchLength < LZS_MAX_MATCH - LZS_BREAK_EVEN && 
                       inPos + matchLength < inSize &&
                       window[(i + matchLength) % LZS_WINDOW_SIZE] == input[inPos + matchLength]) {
                    matchLength++;
                }
                
                /* Keep track of the best match */
                if (matchLength > bestLength) {
                    bestLength = matchLength;
                    bestOffset = i;
                    
                    /* Stop if we've found a very good match */
                    if (bestLength >= LZS_MAX_MATCH - LZS_BREAK_EVEN) {
                        break;
                    }
                }
            }
        }
        
        /* Determine whether to output a literal or a match */
        if (bestLength >= LZS_BREAK_EVEN) {
            /* Output a match */
            output[flagPos] |= flagBit; /* Set flag bit for match */
            
            /* Output match offset and length */
            if (outPos + 1 >= *outSize) return LH_ERR_NO_MEMORY;
            
            output[outPos++] = bestOffset & 0xFF;
            output[outPos++] = ((bestOffset >> 4) & 0xF0) | ((bestLength - LZS_BREAK_EVEN) & 0x0F);
            
            /* Update window with match data */
            for (i = 0; i < bestLength; i++) {
                window[windowPos] = input[inPos];
                windowPos = (windowPos + 1) % LZS_WINDOW_SIZE;
                inPos++;
            }
        } else {
            /* Output a literal byte (flag bit already clear) */
            if (outPos >= *outSize) return LH_ERR_NO_MEMORY;
            output[outPos++] = input[inPos];
            
            /* Update window */
            window[windowPos] = input[inPos];
            windowPos = (windowPos + 1) % LZS_WINDOW_SIZE;
            inPos++;
        }
        
        /* Move to next flag bit */
        flagBit >>= 1;
    }
    
    /* Update output size */
    *outSize = outPos;
    
    return LH_OK;
}

/* Helper function to check buffer boundaries */
static long CheckBufferBounds(unsigned long pos, unsigned long size, const char *operation) {
    if (pos >= size) {
        return LH_ERR_INVALID_ARG;
    }
    return LH_OK;
}

/* Helper function to allocate and initialize lookup tables */
static struct lh_code_lookup *AllocLookupTable(unsigned long size) {
    struct lh_code_lookup *table = malloc(size * sizeof(struct lh_code_lookup));
    if (table) {
        memset(table, 0, size * sizeof(struct lh_code_lookup));
    }
    return table;
}

/* Helper function to free lookup tables */
static void FreeLookupTable(struct lh_code_lookup *table, unsigned long size) {
    if (table) {
        free(table);
    }
}

/* Helper function to update frequency counts */
static void UpdateFrequency(struct lh_code_lookup *table, unsigned long index) {
    if (table && index < LH_MAX_HUFFMAN_LEAVES) {
        table[index].freq++;
    }
}

/* Helper function to find best match */
static unsigned long FindBestMatch(unsigned char *window, unsigned long windowPos, unsigned char *input, unsigned long inputPos, 
                          unsigned long maxMatch, unsigned long dictSize) {
    unsigned long bestLength = 0;
    unsigned long bestOffset = 0;
    unsigned long i;
    unsigned long matchLength;
    
    for (i = 0; i < dictSize; i++) {
        matchLength = 0;
        while (matchLength < maxMatch && 
               input[inputPos + matchLength] == window[(i + matchLength) % dictSize]) {
            matchLength++;
        }
        
        if (matchLength > bestLength) {
            bestLength = matchLength;
            bestOffset = i;
        }
    }
    
    return bestLength;
}

/* LH2 decompression - 8 KiB sliding window, max 256 bytes match length, Dynamic Huffman */
/* -lh2- is rare/experimental; stub avoids crashing on a 4K ring overrun. */
long lh_decompress_lh2(void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize) {
    (void)inBuf; (void)inSize; (void)outBuf; (void)outSize;
    return LH_ERR_UNSUPPORTED;
}

/* LH2 compression - 8 KiB sliding window, max 256 bytes match length, Dynamic Huffman */
long lh_compress_lh2(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long i;
    unsigned char *window = NULL;
    unsigned long windowPos = 0;
    unsigned long literalCount = 0;
    struct lh_code_lookup *codeLookup = NULL;
    struct lh_code_lookup *distLookup = NULL;
    long result = LH_OK;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough */
    if (*outSize < inSize + (inSize / 100) + 12) {
        return LH_ERR_NO_MEMORY;
    }
    
    window = (unsigned char *)malloc(8192u);
    if (!window) {
        return LH_ERR_NO_MEMORY;
    }
    codeLookup = (struct lh_code_lookup *)malloc(
        (256u + LH_MAX_MATCH_LENGTH) * sizeof(struct lh_code_lookup));
    if (!codeLookup) {
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    distLookup = (struct lh_code_lookup *)malloc(
        (8192u + 1u) * sizeof(struct lh_code_lookup));
    if (!distLookup) {
        free(codeLookup);
        free(window);
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize the ring buffer with spaces */
    memset(window, ' ', 8192);
    
    /* First pass: Build frequency tables by scanning input */
    memset(codeLookup, 0, (256u + LH_MAX_MATCH_LENGTH) * sizeof(struct lh_code_lookup));
    memset(distLookup, 0, (8192u + 1u) * sizeof(struct lh_code_lookup));
    
    /* Process input data */
    for (i = 0; i < inSize; i++) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        
        /* Look for matches in the window */
        if (i > MATCH_LENGTH_THRESHOLD) {
            bestLength = FindBestMatch(window, windowPos, input, i, 256, 8192);
            if (bestLength >= MATCH_LENGTH_THRESHOLD) {
                /* Found a match */
                codeLookup[256 + bestLength - MATCH_LENGTH_THRESHOLD].freq++;
                distLookup[bestOffset].freq++;
            } else {
                /* No match, literal */
                codeLookup[input[i]].freq++;
            }
        } else {
            /* Literal */
            codeLookup[input[i]].freq++;
        }
        
        /* Update window */
        window[windowPos] = input[i];
        windowPos = (windowPos + 1) % 8192;
    }
    
    /* Build Huffman trees and compress */
    /* This is a simplified implementation - full implementation would build
       optimal Huffman trees and encode the data */
    
    /* For now, just copy the data (store method) */
    for (i = 0; i < inSize; i++) {
        if (outPos >= *outSize) {
            result = LH_ERR_NO_MEMORY;
            goto cleanup;
        }
        output[outPos++] = input[i];
    }
    
    *outSize = outPos;
    
cleanup:
    if (distLookup) {
        free(distLookup);
    }
    if (codeLookup) {
        free(codeLookup);
    }
    if (window) {
        free(window);
    }
    return result;
}

/* LH3 decompression - LH2 variant with Static Huffman */
long lh_decompress_lh3(void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize) {
    (void)inBuf; (void)inSize; (void)outBuf; (void)outSize;
    return LH_ERR_UNSUPPORTED;
}

/* LH3 compression - LH2 variant with Static Huffman */
long lh_compress_lh3(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    /* LH3 is similar to LH2 but uses static Huffman coding */
    /* For now, use LH2 implementation as base */
    return lh_compress_lh2(inBuf, inSize, outBuf, outSize);
}

/* LH4 decompression - 4 KiB sliding window, max 256 bytes match length, Static Huffman */
/* lh_decompress_lh4 is implemented in lh_lh5dec.c (real LH4..LH7 decoder). */

/* LH4 compression - 4 KiB sliding window, max 256 bytes match length, Static Huffman */
long lh_compress_lh4(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    /* LH4 is similar to LH1 but uses static Huffman coding */
    /* For now, use LH1 implementation as base */
    return lh_compress_lh1(inBuf, inSize, outBuf, outSize);
}

/* LZ5 decompression - 4 KiB sliding window, max 17 bytes match length (LArc format) */
long lh_decompress_lz5(void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long inPos = 0;
    unsigned long i, j;
    unsigned char window[4096];  /* 4K sliding window */
    unsigned long windowPos = 0;
    unsigned char flagByte = 0;
    unsigned long flagPos = 0;
    unsigned long flagBit = 0x80;
    unsigned char offset;
    unsigned char length;
    unsigned char b;
    unsigned long matchOffset;
    unsigned long matchLength;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || outSize == 0) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Initialize window with zeros */
    memset(window, 0, 4096);
    
    /* Process input data */
    while (inPos < inSize && outPos < outSize) {
        /* Check if we need a new flag byte */
        if (flagBit == 0) {
            if (inPos >= inSize) break;
            flagByte = input[inPos++];
            flagBit = 0x80;
        }
        
        /* Check flag bit */
        if (flagByte & flagBit) {
            /* Match */
            if (inPos + 1 >= inSize) break;
            
            offset = input[inPos++];
            length = input[inPos++];
            
            /* LZ5 uses 12-bit offset and 4-bit length */
            matchOffset = ((length & 0xF0) << 4) | offset;
            matchLength = (length & 0x0F) + 3;  /* 3-18 bytes */
            
            /* Copy bytes from window to output */
            for (i = 0; i < matchLength && outPos < outSize; i++) {
                b = window[(matchOffset + i) % 4096];
                output[outPos++] = b;
                
                /* Add to window */
                window[windowPos] = b;
                windowPos = (windowPos + 1) % 4096;
            }
        } else {
            /* Literal byte */
            if (inPos >= inSize) break;
            
            b = input[inPos++];
            if (outPos < outSize) {
                output[outPos++] = b;
                
                /* Add to window */
                window[windowPos] = b;
                windowPos = (windowPos + 1) % 4096;
            }
        }
        
        /* Move to next flag bit */
        flagBit >>= 1;
    }
    
    /* Return success if we decompressed the expected size */
    return (outPos == outSize) ? LH_OK : LH_ERR_INVALID_ARG;
}

/* LZ5 compression - 4 KiB sliding window, max 17 bytes match length (LArc format) */
long lh_compress_lz5(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long outPos = 0;
    unsigned long inPos = 0;
    unsigned long i, j;
    unsigned char window[4096];  /* 4K sliding window */
    unsigned long windowPos = 0;
    unsigned char flagByte = 0;
    unsigned long flagPos = 0;
    unsigned long flagBit = 0x80;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Ensure output buffer is large enough */
    if (*outSize < inSize + (inSize / 8) + 12) {
        return LH_ERR_NO_MEMORY;
    }
    
    /* Initialize window with zeros */
    memset(window, 0, 4096);
    
    /* Reserve space for the first flag byte */
    if (outPos >= *outSize) return LH_ERR_NO_MEMORY;
    flagPos = outPos++;
    output[flagPos] = 0;
    
    /* Process input data */
    while (inPos < inSize) {
        unsigned long bestLength = 0;
        unsigned long bestOffset = 0;
        
        /* Check if we need a new flag byte */
        if (flagBit == 0) {
            if (outPos >= *outSize) return LH_ERR_NO_MEMORY;
            flagPos = outPos++;
            output[flagPos] = 0;
            flagBit = 0x80;
        }
        
        /* Look for the longest match in the window */
        if (inPos > 3) {
            for (i = 0; i < 4096; i++) {
                unsigned long matchLength = 0;
                
                /* Check how many bytes match (max 17 bytes) */
                while (matchLength < 17 && 
                       inPos + matchLength < inSize &&
                       window[(i + matchLength) % 4096] == input[inPos + matchLength]) {
                    matchLength++;
                }
                
                /* Keep track of the best match */
                if (matchLength > bestLength) {
                    bestLength = matchLength;
                    bestOffset = i;
                    
                    /* Stop if we've found a very good match */
                    if (bestLength >= 17) {
                        break;
                    }
                }
            }
        }
        
        /* Determine whether to output a literal or a match */
        if (bestLength >= 3) {
            /* Output a match */
            output[flagPos] |= flagBit; /* Set flag bit for match */
            
            /* Output match offset and length (12-bit offset, 4-bit length) */
            if (outPos + 1 >= *outSize) return LH_ERR_NO_MEMORY;
            
            output[outPos++] = bestOffset & 0xFF;
            output[outPos++] = ((bestOffset >> 4) & 0xF0) | ((bestLength - 3) & 0x0F);
            
            /* Update window with match data */
            for (i = 0; i < bestLength; i++) {
                window[windowPos] = input[inPos];
                windowPos = (windowPos + 1) % 4096;
                inPos++;
            }
        } else {
            /* Output a literal byte (flag bit already clear) */
            if (outPos >= *outSize) return LH_ERR_NO_MEMORY;
            output[outPos++] = input[inPos];
            
            /* Update window */
            window[windowPos] = input[inPos];
            windowPos = (windowPos + 1) % 4096;
            inPos++;
        }
        
        /* Move to next flag bit */
        flagBit >>= 1;
    }
    
    /* Update output size */
    *outSize = outPos;
    
    return LH_OK;
}

/* LZ4 decompression - No compression (LArc format) */
long lh_decompress_lz4(void * inBuf, unsigned long inSize, void * outBuf, unsigned long outSize) {
    /* LZ4 is just a copy operation - no compression */
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long i;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || outSize == 0 || inSize != outSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Copy data */
    for (i = 0; i < inSize; i++) {
        output[i] = input[i];
    }
    
    return LH_OK;
}

/* LZ4 compression - No compression (LArc format) */
long lh_compress_lz4(void * inBuf, unsigned long inSize, void * outBuf, unsigned long *outSize) {
    /* LZ4 is just a copy operation - no compression */
    unsigned char *input = (unsigned char *)inBuf;
    unsigned char *output = (unsigned char *)outBuf;
    unsigned long i;
    
    /* Check parameters */
    if (!inBuf || !outBuf || inSize == 0 || *outSize < inSize) {
        return LH_ERR_INVALID_ARG;
    }
    
    /* Copy data */
    for (i = 0; i < inSize; i++) {
        output[i] = input[i];
    }
    
    *outSize = inSize;
    return LH_OK;
}