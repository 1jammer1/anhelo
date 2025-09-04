#ifndef SIMPLE_H264_H
#define SIMPLE_H264_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// H.264 decoder result codes
typedef enum {
    SIMPLE_H264_OK = 0,
    SIMPLE_H264_ERROR = 1,
    SIMPLE_H264_NEED_MORE_DATA = 2,
    SIMPLE_H264_FRAME_READY = 3,
    SIMPLE_H264_HEADERS_READY = 4,
    SIMPLE_H264_PARAM_SET_ERROR = 5
} simple_h264_result_t;

// H.264 NAL unit types
typedef enum {
    SIMPLE_H264_NAL_UNKNOWN = 0,
    SIMPLE_H264_NAL_SLICE = 1,
    SIMPLE_H264_NAL_DPA = 2,
    SIMPLE_H264_NAL_DPB = 3,
    SIMPLE_H264_NAL_DPC = 4,
    SIMPLE_H264_NAL_IDR_SLICE = 5,
    SIMPLE_H264_NAL_SEI = 6,
    SIMPLE_H264_NAL_SPS = 7,
    SIMPLE_H264_NAL_PPS = 8,
    SIMPLE_H264_NAL_AUD = 9,
    SIMPLE_H264_NAL_END_SEQUENCE = 10,
    SIMPLE_H264_NAL_END_STREAM = 11,
    SIMPLE_H264_NAL_FILLER_DATA = 12
} simple_h264_nal_type_t;

// Forward declaration
typedef struct simple_h264_decoder simple_h264_decoder_t;

// Frame information
typedef struct {
    uint8_t *y_plane;
    uint8_t *u_plane;
    uint8_t *v_plane;
    uint32_t width;
    uint32_t height;
    uint32_t y_stride;
    uint32_t uv_stride;
} simple_h264_frame_t;

// Decoder functions
simple_h264_decoder_t* simple_h264_create(void);
void simple_h264_destroy(simple_h264_decoder_t *decoder);

simple_h264_result_t simple_h264_decode(
    simple_h264_decoder_t *decoder,
    const uint8_t *data,
    size_t data_size,
    simple_h264_frame_t *frame
);

// Utility functions
simple_h264_nal_type_t simple_h264_get_nal_type(const uint8_t *data);
int simple_h264_is_parameter_set(simple_h264_nal_type_t nal_type);
const char* simple_h264_result_string(simple_h264_result_t result);

#ifdef __cplusplus
}
#endif

#endif // SIMPLE_H264_H
