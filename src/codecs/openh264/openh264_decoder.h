#ifndef OPENH264_DECODER_H
#define OPENH264_DECODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decoder status codes
typedef enum {
    OPENH264_SUCCESS = 0,
    OPENH264_ERROR = 1,
    OPENH264_PARAM_SET_ERROR = 2,
    OPENH264_HDRS_RDY = 3,
    OPENH264_PIC_RDY = 4,
    OPENH264_NO_OUTPUT = 5
} openh264_status_t;

// YUV420P frame data
typedef struct {
    uint8_t *y_plane;     // Y (luma) plane
    uint8_t *u_plane;     // U (chroma) plane  
    uint8_t *v_plane;     // V (chroma) plane
    int width;            // Frame width
    int height;           // Frame height
    int y_stride;         // Y plane stride
    int uv_stride;        // UV planes stride
    uint64_t timestamp;   // Presentation timestamp
} openh264_frame_t;

// Opaque decoder handle
typedef struct openh264_decoder_ctx openh264_decoder_ctx_t;

// Initialize decoder
openh264_decoder_ctx_t* openh264_decoder_init(void);

// Destroy decoder
void openh264_decoder_destroy(openh264_decoder_ctx_t* ctx);

// Decode H.264 NAL unit
openh264_status_t openh264_decode_nal(
    openh264_decoder_ctx_t* ctx,
    const uint8_t* nal_data,
    size_t nal_len,
    openh264_frame_t* frame_out
);

// Reset decoder state
void openh264_decoder_reset(openh264_decoder_ctx_t* ctx);

// Check if decoder has parameter sets
int openh264_has_param_sets(openh264_decoder_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif // OPENH264_DECODER_H
