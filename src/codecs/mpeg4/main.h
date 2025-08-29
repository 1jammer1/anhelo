#ifndef MPEG4_DECODER_H
#define MPEG4_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef enum {
    MPEG4_SUCCESS = 0,
    MPEG4_ERROR_INVALID_PARAM = -1,
    MPEG4_ERROR_MEMORY = -2,
    MPEG4_ERROR_BITSTREAM = -3,
    MPEG4_ERROR_UNSUPPORTED = -4
} mpeg4_error_t;

// Decoder context
typedef struct mpeg4_decoder mpeg4_decoder_t;

// Create decoder instance
mpeg4_decoder_t* mpeg4_create_decoder(int width, int height);

// Destroy decoder instance
void mpeg4_destroy_decoder(mpeg4_decoder_t* decoder);

// Decode a single frame
mpeg4_error_t mpeg4_decode_frame(mpeg4_decoder_t* decoder,
                                const uint8_t* bitstream,
                                size_t bitstream_size,
                                uint8_t** y_plane,
                                uint8_t** u_plane,
                                uint8_t** v_plane,
                                int* stride_y,
                                int* stride_uv);

// Get frame dimensions
void mpeg4_get_frame_size(mpeg4_decoder_t* decoder, int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif // MPEG4_DECODER_H