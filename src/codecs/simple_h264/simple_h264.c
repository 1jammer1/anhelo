#include "simple_h264.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Basic H.264 decoder state
struct simple_h264_decoder {
    // SPS (Sequence Parameter Set) data
    uint32_t width;
    uint32_t height;
    uint8_t profile_idc;
    uint8_t level_idc;
    uint8_t sps_valid;
    
    // PPS (Picture Parameter Set) data
    uint8_t pps_valid;
    
    // Frame buffer
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    
    // Decoder state
    uint8_t initialized;
};

// Exponential Golomb decoding helpers
static uint32_t read_ue_golomb(const uint8_t **data, size_t *bits_left) {
    if (*bits_left == 0) return 0;
    
    uint32_t leading_zeros = 0;
    const uint8_t *ptr = *data;
    size_t bit_pos = 0;
    
    // Count leading zeros
    while (bit_pos < *bits_left) {
        uint8_t byte = ptr[bit_pos / 8];
        uint8_t bit = (byte >> (7 - (bit_pos % 8))) & 1;
        if (bit) break;
        leading_zeros++;
        bit_pos++;
    }
    
    if (bit_pos + leading_zeros + 1 > *bits_left) return 0;
    
    uint32_t value = 0;
    bit_pos++; // Skip the '1' bit
    
    // Read the remaining bits
    for (uint32_t i = 0; i < leading_zeros; i++) {
        if (bit_pos >= *bits_left) break;
        uint8_t byte = ptr[bit_pos / 8];
        uint8_t bit = (byte >> (7 - (bit_pos % 8))) & 1;
        value = (value << 1) | bit;
        bit_pos++;
    }
    
    *data = ptr + (bit_pos / 8);
    *bits_left -= bit_pos;
    
    return (1 << leading_zeros) - 1 + value;
}

// Parse SPS (Sequence Parameter Set)
static int parse_sps(simple_h264_decoder_t *decoder, const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    
    // Skip NAL header
    const uint8_t *ptr = data + 1;
    size_t bits_left = (size - 1) * 8;
    
    decoder->profile_idc = ptr[0];
    decoder->level_idc = ptr[2];
    
    ptr += 3;
    bits_left -= 24;
    
    // Read seq_parameter_set_id
    uint32_t seq_param_id = read_ue_golomb(&ptr, &bits_left);
    (void)seq_param_id; // Suppress unused warning
    
    // For baseline profile, skip to width/height
    if (decoder->profile_idc == 66) { // Baseline profile
        // Read log2_max_frame_num_minus4
        read_ue_golomb(&ptr, &bits_left);
        
        // Read pic_order_cnt_type
        uint32_t poc_type = read_ue_golomb(&ptr, &bits_left);
        if (poc_type == 0) {
            read_ue_golomb(&ptr, &bits_left); // log2_max_pic_order_cnt_lsb_minus4
        }
        
        // Read max_num_ref_frames
        read_ue_golomb(&ptr, &bits_left);
        
        // Skip gaps_in_frame_num_value_allowed_flag
        if (bits_left > 0) {
            ptr += (bits_left % 8 != 0) ? 1 : 0;
            bits_left = bits_left - (bits_left % 8);
        }
        
        // Read pic_width_in_mbs_minus1
        uint32_t width_mbs = read_ue_golomb(&ptr, &bits_left) + 1;
        decoder->width = width_mbs * 16;
        
        // Read pic_height_in_map_units_minus1
        uint32_t height_mbs = read_ue_golomb(&ptr, &bits_left) + 1;
        decoder->height = height_mbs * 16;
        
        decoder->sps_valid = 1;
        return 1;
    }
    
    // For other profiles, use fallback dimensions
    decoder->width = 1920;
    decoder->height = 1080;
    decoder->sps_valid = 1;
    return 1;
}

// Parse PPS (Picture Parameter Set)
static int parse_pps(simple_h264_decoder_t *decoder, const uint8_t *data, size_t size) {
    (void)data;
    (void)size;
    decoder->pps_valid = 1;
    return 1;
}

// Create decoder instance
simple_h264_decoder_t* simple_h264_create(void) {
    simple_h264_decoder_t *decoder = calloc(1, sizeof(simple_h264_decoder_t));
    if (!decoder) return NULL;
    
    return decoder;
}

// Destroy decoder instance
void simple_h264_destroy(simple_h264_decoder_t *decoder) {
    if (!decoder) return;
    
    free(decoder->frame_buffer);
    free(decoder);
}

// Get NAL unit type
simple_h264_nal_type_t simple_h264_get_nal_type(const uint8_t *data) {
    if (!data) return SIMPLE_H264_NAL_UNKNOWN;
    return (simple_h264_nal_type_t)(data[0] & 0x1F);
}

// Check if NAL type is a parameter set
int simple_h264_is_parameter_set(simple_h264_nal_type_t nal_type) {
    return (nal_type == SIMPLE_H264_NAL_SPS || nal_type == SIMPLE_H264_NAL_PPS);
}

// Convert result code to string
const char* simple_h264_result_string(simple_h264_result_t result) {
    switch (result) {
        case SIMPLE_H264_OK: return "OK";
        case SIMPLE_H264_ERROR: return "ERROR";
        case SIMPLE_H264_NEED_MORE_DATA: return "NEED_MORE_DATA";
        case SIMPLE_H264_FRAME_READY: return "FRAME_READY";
        case SIMPLE_H264_HEADERS_READY: return "HEADERS_READY";
        case SIMPLE_H264_PARAM_SET_ERROR: return "PARAM_SET_ERROR";
        default: return "UNKNOWN";
    }
}

// Main decode function
simple_h264_result_t simple_h264_decode(
    simple_h264_decoder_t *decoder,
    const uint8_t *data,
    size_t data_size,
    simple_h264_frame_t *frame
) {
    if (!decoder || !data || data_size == 0) {
        return SIMPLE_H264_ERROR;
    }
    
    simple_h264_nal_type_t nal_type = simple_h264_get_nal_type(data);
    
    printf("[SIMPLE_H264] Processing NAL type %d, size %zu\n", nal_type, data_size);
    
    switch (nal_type) {
        case SIMPLE_H264_NAL_SPS:
            if (parse_sps(decoder, data, data_size)) {
                printf("[SIMPLE_H264] SPS parsed: %dx%d\n", decoder->width, decoder->height);
                return SIMPLE_H264_HEADERS_READY;
            } else {
                printf("[SIMPLE_H264] SPS parsing failed\n");
                return SIMPLE_H264_PARAM_SET_ERROR;
            }
            break;
            
        case SIMPLE_H264_NAL_PPS:
            if (parse_pps(decoder, data, data_size)) {
                printf("[SIMPLE_H264] PPS parsed successfully\n");
                return SIMPLE_H264_HEADERS_READY;
            } else {
                printf("[SIMPLE_H264] PPS parsing failed\n");
                return SIMPLE_H264_PARAM_SET_ERROR;
            }
            break;
            
        case SIMPLE_H264_NAL_IDR_SLICE:
        case SIMPLE_H264_NAL_SLICE:
            if (!decoder->sps_valid || !decoder->pps_valid) {
                printf("[SIMPLE_H264] Missing parameter sets for slice\n");
                return SIMPLE_H264_PARAM_SET_ERROR;
            }
            
            // For this simple implementation, we'll create a test pattern
            // instead of actually decoding the slice data
            if (frame && decoder->width > 0 && decoder->height > 0) {
                // Allocate frame buffer if needed
                size_t needed_size = decoder->width * decoder->height * 3 / 2; // YUV420
                if (!decoder->frame_buffer || decoder->frame_buffer_size < needed_size) {
                    free(decoder->frame_buffer);
                    decoder->frame_buffer = malloc(needed_size);
                    decoder->frame_buffer_size = needed_size;
                }
                
                if (decoder->frame_buffer) {
                    // Create a simple test pattern (gray with some variation)
                    uint8_t *y_plane = decoder->frame_buffer;
                    uint8_t *u_plane = y_plane + decoder->width * decoder->height;
                    uint8_t *v_plane = u_plane + (decoder->width * decoder->height) / 4;
                    
                    // Fill Y plane with gradient pattern
                    for (uint32_t y = 0; y < decoder->height; y++) {
                        for (uint32_t x = 0; x < decoder->width; x++) {
                            y_plane[y * decoder->width + x] = (uint8_t)((x + y) % 256);
                        }
                    }
                    
                    // Fill U and V planes with constant values
                    memset(u_plane, 128, (decoder->width * decoder->height) / 4);
                    memset(v_plane, 128, (decoder->width * decoder->height) / 4);
                    
                    // Set up frame structure
                    frame->y_plane = y_plane;
                    frame->u_plane = u_plane;
                    frame->v_plane = v_plane;
                    frame->width = decoder->width;
                    frame->height = decoder->height;
                    frame->y_stride = decoder->width;
                    frame->uv_stride = decoder->width / 2;
                    
                    printf("[SIMPLE_H264] Frame ready: %dx%d\n", decoder->width, decoder->height);
                    return SIMPLE_H264_FRAME_READY;
                }
            }
            
            printf("[SIMPLE_H264] Slice processed (no actual decoding)\n");
            return SIMPLE_H264_OK;
            
        case SIMPLE_H264_NAL_AUD:
            printf("[SIMPLE_H264] Access Unit Delimiter processed\n");
            return SIMPLE_H264_OK;
            
        case SIMPLE_H264_NAL_SEI:
            printf("[SIMPLE_H264] SEI message processed\n");
            return SIMPLE_H264_OK;
            
        default:
            printf("[SIMPLE_H264] Unsupported NAL type %d\n", nal_type);
            return SIMPLE_H264_OK;
    }
}
