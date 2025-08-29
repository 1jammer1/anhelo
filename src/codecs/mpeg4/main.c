#include "main.h"
#include <stdlib.h>
#include <string.h>


typedef struct {
    uint32_t cache;
    int bits_left;
    const uint8_t* data;
    const uint8_t* end;
    const uint8_t* ptr;
} bit_reader_t;


struct mpeg4_decoder {
    int width;
    int height;
    int mb_width;
    int mb_height;
    int stride_y;
    int stride_uv;
    
    
    uint8_t* frame_memory;
    uint8_t* y_plane;
    uint8_t* u_plane;
    uint8_t* v_plane;
    
    
    short blocks[6][64];
};


static void init_bit_reader(bit_reader_t* br, const uint8_t* data, size_t size) {
    br->data = data;
    br->ptr = data;
    br->end = data + size;
    br->cache = 0;
    br->bits_left = 0;
}

static inline int get_bits(bit_reader_t* br, int n) {
    while (br->bits_left < n) {
        if (br->ptr < br->end) {
            br->cache |= ((uint32_t)(*br->ptr++)) << (24 - br->bits_left);
            br->bits_left += 8;
        } else {
            return 0; 
        }
    }
    
    int result = br->cache >> (32 - n);
    br->cache <<= n;
    br->bits_left -= n;
    return result;
}

static inline int peek_bits(bit_reader_t* br, int n) {
    while (br->bits_left < n) {
        if (br->ptr < br->end) {
            br->cache |= ((uint32_t)(*br->ptr++)) << (24 - br->bits_left);
            br->bits_left += 8;
        } else {
            break;
        }
    }
    return br->cache >> (32 - n);
}

static inline void skip_bits(bit_reader_t* br, int n) {
    while (br->bits_left < n) {
        if (br->ptr < br->end) {
            br->cache |= ((uint32_t)(*br->ptr++)) << (24 - br->bits_left);
            br->bits_left += 8;
        } else {
            br->bits_left = 0;
            br->cache = 0;
            return;
        }
    }
    br->cache <<= n;
    br->bits_left -= n;
}


static void simple_idct(short* block) {
    
    for (int i = 0; i < 64; i++) {
        block[i] = (block[i] * 4) >> 3; 
    }
}


static void add_block_to_frame(mpeg4_decoder_t* dec, short* block, int x, int y, int comp) {
    uint8_t* dst;
    int stride;
    int block_size = 8;
    
    if (comp == 0) { 
        dst = dec->y_plane + y * dec->stride_y + x;
        stride = dec->stride_y;
    } else if (comp == 1) { 
        dst = dec->u_plane + (y >> 1) * dec->stride_uv + (x >> 1);
        stride = dec->stride_uv;
        block_size = 4; 
    } else { 
        dst = dec->v_plane + (y >> 1) * dec->stride_uv + (x >> 1);
        stride = dec->stride_uv;
        block_size = 4;
    }
    
    
    for (int j = 0; j < block_size; j++) {
        for (int i = 0; i < block_size; i++) {
            int idx = j * 8 + i;
            int val = (block[idx] + 128);
            if (val < 0) val = 0;
            else if (val > 255) val = 255;
            dst[j * stride + i] = (uint8_t)val;
        }
    }
}


static void decode_macroblock(mpeg4_decoder_t* dec, bit_reader_t* br, int mb_x, int mb_y) {
    
    memset(dec->blocks, 0, sizeof(dec->blocks));
    
    
    for (int i = 0; i < 6; i++) {
        dec->blocks[i][0] = get_bits(br, 10);
    }
    
    
    for (int i = 0; i < 6; i++) {
        for (int j = 1; j < 16; j++) { 
            if (peek_bits(br, 1)) {
                skip_bits(br, 1);
                int coeff = get_bits(br, 8);
                dec->blocks[i][j] = (coeff & 1) ? -(coeff >> 1) : (coeff >> 1);
            } else {
                skip_bits(br, 1);
            }
        }
        simple_idct(dec->blocks[i]);
    }
    
    
    int x = mb_x * 16;
    int y = mb_y * 16;
    
    
    add_block_to_frame(dec, dec->blocks[0], x, y, 0);
    add_block_to_frame(dec, dec->blocks[1], x + 8, y, 0);
    add_block_to_frame(dec, dec->blocks[2], x, y + 8, 0);
    add_block_to_frame(dec, dec->blocks[3], x + 8, y + 8, 0);
    
    
    add_block_to_frame(dec, dec->blocks[4], x, y, 1);
    add_block_to_frame(dec, dec->blocks[5], x, y, 2);
}


mpeg4_decoder_t* mpeg4_create_decoder(int width, int height) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        return NULL;
    }
    
    mpeg4_decoder_t* dec = (mpeg4_decoder_t*)calloc(1, sizeof(mpeg4_decoder_t));
    if (!dec) return NULL;
    
    
    dec->width = (width + 15) & ~15;
    dec->height = (height + 15) & ~15;
    dec->mb_width = dec->width / 16;
    dec->mb_height = dec->height / 16;
    dec->stride_y = dec->width;
    dec->stride_uv = dec->width / 2;
    
    
    size_t y_size = dec->stride_y * dec->height;
    size_t uv_size = dec->stride_uv * (dec->height / 2);
    size_t total_size = y_size + 2 * uv_size;
    
    
    dec->frame_memory = (uint8_t*)malloc(total_size);
    if (!dec->frame_memory) {
        free(dec);
        return NULL;
    }
    
    
    dec->y_plane = dec->frame_memory;
    dec->u_plane = dec->y_plane + y_size;
    dec->v_plane = dec->u_plane + uv_size;
    
    
    memset(dec->y_plane, 0, y_size);
    memset(dec->u_plane, 128, uv_size);
    memset(dec->v_plane, 128, uv_size);
    
    return dec;
}

void mpeg4_destroy_decoder(mpeg4_decoder_t* decoder) {
    if (decoder) {
        if (decoder->frame_memory) {
            free(decoder->frame_memory);
        }
        free(decoder);
    }
}

mpeg4_error_t mpeg4_decode_frame(mpeg4_decoder_t* decoder,
                                const uint8_t* bitstream,
                                size_t bitstream_size,
                                uint8_t** y_plane,
                                uint8_t** u_plane,
                                uint8_t** v_plane,
                                int* stride_y,
                                int* stride_uv) {
    if (!decoder || !bitstream || !y_plane || !u_plane || !v_plane || 
        !stride_y || !stride_uv || bitstream_size == 0) {
        return MPEG4_ERROR_INVALID_PARAM;
    }
    
    bit_reader_t reader;
    init_bit_reader(&reader, bitstream, bitstream_size);
    
    
    skip_bits(&reader, 32); 
    skip_bits(&reader, 2);  
    
    
    for (int mb_y = 0; mb_y < decoder->mb_height; mb_y++) {
        for (int mb_x = 0; mb_x < decoder->mb_width; mb_x++) {
            if (reader.ptr >= reader.end) {
                
                break;
            }
            decode_macroblock(decoder, &reader, mb_x, mb_y);
        }
    }
    
    
    *y_plane = decoder->y_plane;
    *u_plane = decoder->u_plane;
    *v_plane = decoder->v_plane;
    *stride_y = decoder->stride_y;
    *stride_uv = decoder->stride_uv;
    
    return MPEG4_SUCCESS;
}

void mpeg4_get_frame_size(mpeg4_decoder_t* decoder, int* width, int* height) {
    if (decoder && width && height) {
        *width = decoder->width;
        *height = decoder->height;
    }
}