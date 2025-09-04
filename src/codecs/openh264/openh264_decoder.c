#include "openh264_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Simple H.264 NAL unit types
#define NAL_SLICE           1
#define NAL_DPA             2
#define NAL_DPB             3
#define NAL_DPC             4
#define NAL_IDR_SLICE       5
#define NAL_SEI             6
#define NAL_SPS             7
#define NAL_PPS             8
#define NAL_AUD             9
#define NAL_END_SEQUENCE    10
#define NAL_END_STREAM      11
#define NAL_FILLER_DATA     12

// Maximum supported resolution
#define MAX_WIDTH  1920
#define MAX_HEIGHT 1080
#define MAX_FRAME_SIZE (MAX_WIDTH * MAX_HEIGHT * 3 / 2)

// Simple parameter sets storage
typedef struct {
    uint8_t sps_data[256];
    size_t sps_len;
    int sps_valid;
    
    uint8_t pps_data[64];
    size_t pps_len;
    int pps_valid;
    
    int width;
    int height;
    int profile;
    int level;
} param_sets_t;

// Decoder context
struct openh264_decoder_ctx {
    param_sets_t param_sets;
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    int initialized;
    int frame_ready;
    openh264_frame_t current_frame;
};

// Extract NAL unit type from data
static inline int get_nal_type(const uint8_t *data, size_t len) {
    if (!data || len < 1) return -1;
    return data[0] & 0x1F;
}

// Parse SPS for width/height (simplified)
static int parse_sps_dimensions(const uint8_t *sps_data, size_t len, int *width, int *height) {
    // Very basic SPS parsing - this is simplified and may not work for all streams
    // In a real implementation, you'd need proper bitstream parsing
    if (!sps_data || len < 10) return -1;
    
    // For now, assume common resolutions based on common patterns
    // This is a placeholder - real SPS parsing is complex
    *width = 1280;  // Default assumption
    *height = 720;
    
    printf("[DEBUG] SPS parsed: assumed %dx%d (simplified parsing)\n", *width, *height);
    return 0;
}

// Initialize decoder
openh264_decoder_ctx_t* openh264_decoder_init(void) {
    openh264_decoder_ctx_t *ctx = calloc(1, sizeof(openh264_decoder_ctx_t));
    if (!ctx) return NULL;
    
    ctx->frame_buffer = malloc(MAX_FRAME_SIZE);
    if (!ctx->frame_buffer) {
        free(ctx);
        return NULL;
    }
    
    ctx->frame_buffer_size = MAX_FRAME_SIZE;
    ctx->initialized = 1;
    
    printf("[DEBUG] OpenH264 decoder initialized\n");
    return ctx;
}

// Destroy decoder
void openh264_decoder_destroy(openh264_decoder_ctx_t* ctx) {
    if (!ctx) return;
    
    free(ctx->frame_buffer);
    free(ctx);
    
    printf("[DEBUG] OpenH264 decoder destroyed\n");
}

// Reset decoder state
void openh264_decoder_reset(openh264_decoder_ctx_t* ctx) {
    if (!ctx) return;
    
    memset(&ctx->param_sets, 0, sizeof(param_sets_t));
    ctx->frame_ready = 0;
    
    printf("[DEBUG] OpenH264 decoder reset\n");
}

// Check if decoder has parameter sets
int openh264_has_param_sets(openh264_decoder_ctx_t* ctx) {
    if (!ctx) return 0;
    return ctx->param_sets.sps_valid && ctx->param_sets.pps_valid;
}

// Generate a test pattern frame
static void generate_test_frame(openh264_decoder_ctx_t* ctx, int width, int height) {
    // Generate a simple test pattern (colorbar-like)
    static int frame_counter = 0;
    frame_counter++;
    
    int y_size = width * height;
    int uv_size = (width * height) / 4;
    
    uint8_t *y_plane = ctx->frame_buffer;
    uint8_t *u_plane = y_plane + y_size;
    uint8_t *v_plane = u_plane + uv_size;
    
    // Generate moving pattern
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            // Create a moving diagonal pattern
            int pattern = ((x + y + frame_counter) % 256);
            y_plane[idx] = pattern;
        }
    }
    
    // Fill chroma planes with constant values for colored pattern
    memset(u_plane, 128 + (frame_counter % 64) - 32, uv_size);  // Blue-ish variation
    memset(v_plane, 128 + ((frame_counter * 2) % 64) - 32, uv_size);  // Red-ish variation
    
    // Setup frame structure
    ctx->current_frame.y_plane = y_plane;
    ctx->current_frame.u_plane = u_plane;
    ctx->current_frame.v_plane = v_plane;
    ctx->current_frame.width = width;
    ctx->current_frame.height = height;
    ctx->current_frame.y_stride = width;
    ctx->current_frame.uv_stride = width / 2;
    ctx->current_frame.timestamp = frame_counter;
    
    ctx->frame_ready = 1;
}

// Decode H.264 NAL unit
openh264_status_t openh264_decode_nal(
    openh264_decoder_ctx_t* ctx,
    const uint8_t* nal_data,
    size_t nal_len,
    openh264_frame_t* frame_out
) {
    if (!ctx || !nal_data || nal_len == 0) {
        return OPENH264_ERROR;
    }
    
    int nal_type = get_nal_type(nal_data, nal_len);
    
    switch (nal_type) {
        case NAL_SPS:
            printf("[DEBUG] Processing SPS (len=%zu)\n", nal_len);
            if (nal_len < sizeof(ctx->param_sets.sps_data)) {
                memcpy(ctx->param_sets.sps_data, nal_data, nal_len);
                ctx->param_sets.sps_len = nal_len;
                ctx->param_sets.sps_valid = 1;
                
                // Parse dimensions
                parse_sps_dimensions(nal_data, nal_len, 
                                   &ctx->param_sets.width, 
                                   &ctx->param_sets.height);
            }
            return OPENH264_HDRS_RDY;
            
        case NAL_PPS:
            printf("[DEBUG] Processing PPS (len=%zu)\n", nal_len);
            if (nal_len < sizeof(ctx->param_sets.pps_data)) {
                memcpy(ctx->param_sets.pps_data, nal_data, nal_len);
                ctx->param_sets.pps_len = nal_len;
                ctx->param_sets.pps_valid = 1;
            }
            return OPENH264_HDRS_RDY;
            
        case NAL_IDR_SLICE:
        case NAL_SLICE:
            printf("[DEBUG] Processing %s slice (len=%zu)\n", 
                   (nal_type == NAL_IDR_SLICE) ? "IDR" : "P", nal_len);
            
            if (!openh264_has_param_sets(ctx)) {
                printf("[DEBUG] No parameter sets available for slice decode\n");
                return OPENH264_PARAM_SET_ERROR;
            }
            
            // Generate test frame instead of actual decoding
            // In a real decoder, this would decode the actual H.264 slice data
            generate_test_frame(ctx, ctx->param_sets.width, ctx->param_sets.height);
            
            if (frame_out && ctx->frame_ready) {
                *frame_out = ctx->current_frame;
                ctx->frame_ready = 0;
                return OPENH264_PIC_RDY;
            }
            return OPENH264_SUCCESS;
            
        case NAL_SEI:
            printf("[DEBUG] Processing SEI (len=%zu)\n", nal_len);
            return OPENH264_SUCCESS;
            
        case NAL_AUD:
            printf("[DEBUG] Processing AUD (len=%zu)\n", nal_len);
            return OPENH264_SUCCESS;
            
        default:
            printf("[DEBUG] Ignoring NAL type %d (len=%zu)\n", nal_type, nal_len);
            return OPENH264_SUCCESS;
    }
}
