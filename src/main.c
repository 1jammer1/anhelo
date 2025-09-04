#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <curl/curl.h>

#ifndef NO_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#endif

#include <SDL/SDL.h>

#include "../include/video.h"
#include "../include/twitch.h"
#include "../include/memory_pool.h"
#include "../include/hls_demuxer.h"

#ifdef USE_OPENH264
#include "codecs/openh264/openh264_decoder.h"
#endif

#include "codecs/simple_h264/simple_h264.h"

#ifdef USE_MPEG4
#include "codecs/mpeg4/main.h"
#endif

// Forward declarations
int init_video_output(int width, int height);
static void yuv420_to_rgb24(int width, int height,
                           const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
                           int y_stride, int u_stride, int v_stride,
                           uint8_t *rgb, int rgb_stride);
static uint64_t get_time_us();
static int process_h264_nal_unit(const uint8_t *nal_data, size_t nal_len, const char *debug_prefix);

#ifdef BACKEND_OPENGL
extern video_t *video_create(int width, int height);
extern void video_draw(video_t *v, const uint8_t *rgb, int linesize);
extern int video_poll(video_t *v);
extern void video_destroy(video_t *v);
#endif

// Global variables for cleanup
#ifndef NO_FFMPEG
static AVFormatContext *format_ctx = NULL;
static AVCodecContext *codec_ctx = NULL;
static struct SwsContext *sws_ctx = NULL;
static AVFrame *frame = NULL;
static AVFrame *rgb_frame = NULL;
static uint8_t *rgb_buffer = NULL;
#else
static uint8_t *rgb_buffer = NULL; // RGB buffer for custom conversion
#endif

static video_t *video = NULL;
static SDL_Surface *screen = NULL;

// Custom decoders
static simple_h264_decoder_t *h264_decoder = NULL;
#ifdef USE_MPEG4
static mpeg4_decoder_t *mpeg4_decoder = NULL;
#endif

// HLS demuxer
static hls_demuxer_t *hls_demuxer = NULL;
static int use_custom_decoder = 0; // 0=FFmpeg, 1=H.264, 2=MPEG-4
static int use_hls_demuxer = 0;
static int should_quit_hls = 0; // Quit flag for HLS playback

// Basic frame structure for custom decoders (when NO_FFMPEG is defined)
#ifdef NO_FFMPEG
/* No frame structs needed in NO_FFMPEG mode; decoders provide planar YUV, we convert to RGB */
#endif

// Memory pools for optimization
static frame_pool_t *frame_pool = NULL;
static memory_pool_t *string_pool = NULL;

// Frame timing variables - optimized for smooth playbook
static uint64_t last_frame_time = 0;
static uint64_t frame_duration_us = 33333; // ~30 FPS in microseconds
#ifndef NO_FFMPEG
static double frame_rate = 30.0; // Default frame rate (FFmpeg mode only)
#endif
static int frames_dropped = 0;
static int frames_displayed = 0;
#ifndef NO_FFMPEG
static int skip_remaining = 0; // Runtime counter: skip this many decoded frames after last displayed frame (FFmpeg mode only)
#endif

// Simple clamp helper
static inline uint8_t clamp_u8(int x) { return (x < 0) ? 0 : (x > 255 ? 255 : (uint8_t)x); }

// Get current time in microseconds
static uint64_t get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

// Convert YUV420P (planar) to RGB24
static void yuv420_to_rgb24(int width, int height,
                            const uint8_t *y_plane, const uint8_t *u_plane, const uint8_t *v_plane,
                            int y_stride, int u_stride, int v_stride,
                            uint8_t *rgb, int rgb_stride)
{
    for (int j = 0; j < height; ++j) {
        const uint8_t *py = y_plane + j * y_stride;
        const uint8_t *pu = u_plane + (j / 2) * u_stride;
        const uint8_t *pv = v_plane + (j / 2) * v_stride;
        uint8_t *prgb = rgb + j * rgb_stride;
        
        for (int i = 0; i < width; ++i) {
            int y = py[i];
            int u = pu[i / 2] - 128;
            int v = pv[i / 2] - 128;
            
            int r = y + ((v * 359) >> 8);
            int g = y - ((u * 88) >> 8) - ((v * 183) >> 8);
            int b = y + ((u * 454) >> 8);
            
            prgb[i * 3 + 0] = clamp_u8(r);
            prgb[i * 3 + 1] = clamp_u8(g);
            prgb[i * 3 + 2] = clamp_u8(b);
        }
    }
}

// NAL unit type detection helper (using simple decoder)
static inline int get_nal_unit_type(const uint8_t *nal_data, size_t nal_len) {
    (void)nal_len; // unused
    return (int)simple_h264_get_nal_type(nal_data);
}

// Check if NAL unit is a parameter set (SPS=7, PPS=8)
static inline int is_parameter_set(int nal_type) {
    return simple_h264_is_parameter_set((simple_h264_nal_type_t)nal_type);
}

// Check if NAL unit is a slice (1-5)
static inline int is_slice(int nal_type) {
    return (nal_type >= 1 && nal_type <= 5);
}

// Enhanced H.264 NAL processing with simple decoder
static int process_h264_nal_unit(const uint8_t *nal_data, size_t nal_len, const char *debug_prefix) {
    if (!nal_data || nal_len == 0 || !h264_decoder) return 0;
    
    simple_h264_nal_type_t nal_type = simple_h264_get_nal_type(nal_data);
    simple_h264_frame_t frame = {0};
    
    simple_h264_result_t result = simple_h264_decode(h264_decoder, nal_data, nal_len, &frame);
    
    // Enhanced debug output with NAL type information
    printf("[DEBUG] %s NAL len=%zu type=%d result=%s pic=%p w=%u h=%u\n", 
           debug_prefix, nal_len, nal_type, simple_h264_result_string(result), 
           (void*)frame.y_plane, frame.width, frame.height);
    fflush(stdout);
    
    // For parameter sets, dump hex data for analysis
    if (simple_h264_is_parameter_set(nal_type)) {
        printf("[DEBUG] ParamSet hex dump (first 16 bytes): ");
        for (size_t i = 0; i < (nal_len < 16 ? nal_len : 16); i++) {
            printf("%02x ", nal_data[i]);
        }
        printf("\n");
    }
    
    // Provide more detailed status information
    if (result == SIMPLE_H264_ERROR) {
        printf("[DEBUG] %s NAL type %d caused decoder error\n", debug_prefix, nal_type);
    } else if (result == SIMPLE_H264_PARAM_SET_ERROR) {
        printf("[DEBUG] %s NAL type %d parameter set error (missing SPS/PPS?)\n", debug_prefix, nal_type);
    } else if (result == SIMPLE_H264_HEADERS_READY) {
        printf("[DEBUG] %s NAL type %d headers ready\n", debug_prefix, nal_type);
    }
    
    if (result == SIMPLE_H264_FRAME_READY && frame.y_plane && frame.width > 0 && frame.height > 0) {
        if (!video) { 
            if (init_video_output(frame.width, frame.height) < 0) { 
                printf("[DEBUG] Failed to initialize video output %ux%u\n", frame.width, frame.height);
                return 0; 
            }
        }
        static int last_w = 0, last_h = 0;
        if (!rgb_buffer || last_w != (int)frame.width || last_h != (int)frame.height) {
            free(rgb_buffer);
            rgb_buffer = (uint8_t*)malloc(frame.width * frame.height * 3);
            if (!rgb_buffer) {
                printf("[DEBUG] Failed to allocate RGB buffer %ux%u\n", frame.width, frame.height);
                return 0;
            }
            last_w = (int)frame.width; last_h = (int)frame.height;
        }
        yuv420_to_rgb24((int)frame.width, (int)frame.height, 
                        frame.y_plane, frame.u_plane, frame.v_plane,
                        (int)frame.y_stride, (int)frame.uv_stride, (int)frame.uv_stride, 
                        rgb_buffer, (int)frame.width * 3);
        video_draw(video, rgb_buffer, (int)frame.width * 3);
        frames_displayed++;
        uint64_t now = get_time_us();
        if (now - last_frame_time < frame_duration_us) usleep(frame_duration_us - (now - last_frame_time));
        last_frame_time = get_time_us();
        if (video && video_poll(video)) { should_quit_hls = 1; return 1; }
        return 1; // Frame displayed
    }
    return 0; // No frame displayed
}

// Smart frame dropping with adaptive thresholds
#ifndef FRAMESKIP_AMOUNT
#define FRAMESKIP_AMOUNT 3
#endif

// HLS segment callback - processes each segment
static int hls_segment_callback(const unsigned char *data, size_t size, void *user_data) {
    (void)user_data; // Not used
    
    // Allow user to quit between segments
    if (video && video_poll(video)) { should_quit_hls = 1; return 1; }

    if (should_quit_hls) return 1;
    // Debug: indicate callback invocation and data size
    printf("[DEBUG] HLS segment callback invoked. size=%zu bytes\n", size);
    fflush(stdout);
    static int pes_dump_done = 0; /* one-time dump flag for assembled PES payload */
    if (use_custom_decoder == 1) {
        // Use simple H.264 decoder
        simple_h264_frame_t frame = {0};
        
        // Debug: about to call decoder
        printf("[DEBUG] Calling simple_h264_decode() with %zu bytes\n", size);
        fflush(stdout);

        simple_h264_result_t result = simple_h264_decode(h264_decoder, data, size, &frame);

        // Debug: report decoder status and picture info
        printf("[DEBUG] simple_h264_decode returned result=%s, picture=%p, width=%u, height=%u\n",
             simple_h264_result_string(result), (void*)frame.y_plane, frame.width, frame.height);
        fflush(stdout);

        if (result == SIMPLE_H264_FRAME_READY && frame.y_plane && frame.width > 0 && frame.height > 0) {
            // Convert YUV to RGB and display
            if (!video) {
                if (init_video_output(frame.width, frame.height) < 0) {
                    return -1;
                }
            }
            
            // Allocate/resize RGB buffer
            static int hls_last_w = 0, hls_last_h = 0;
            if (!rgb_buffer || hls_last_w != (int)frame.width || hls_last_h != (int)frame.height) {
                free(rgb_buffer);
                rgb_buffer = (uint8_t *)malloc(frame.width * frame.height * 3);
                if (!rgb_buffer) { fprintf(stderr, "Failed to allocate RGB buffer (HLS)\n"); return -1; }
                hls_last_w = (int)frame.width; hls_last_h = (int)frame.height;
            }

            // YUV420P layout from simple decoder: separate Y, U, V planes
            yuv420_to_rgb24((int)frame.width, (int)frame.height, 
                          frame.y_plane, frame.u_plane, frame.v_plane,
                          (int)frame.y_stride, (int)frame.uv_stride, (int)frame.uv_stride,
                          rgb_buffer, (int)frame.width * 3);

            video_draw(video, rgb_buffer, (int)frame.width * 3);
            frames_displayed++;
            
            // Handle timing
            uint64_t current_time = get_time_us();
            if (current_time - last_frame_time < frame_duration_us) {
                usleep(frame_duration_us - (current_time - last_frame_time));
            }
            last_frame_time = get_time_us();

            // Poll for quit events after displaying a frame
            if (video && video_poll(video)) { should_quit_hls = 1; return 1; }
        }
        else {
            // Debug: decoder did not produce a ready picture for this segment
            printf("[DEBUG] Decoder did not produce picture ready for this segment (result=%s)\n", simple_h264_result_string(result));
            fflush(stdout);

            /* Enhanced TS demux: many HLS segments are MPEG-TS files (188-byte packets).
             * Try a best-effort extraction of PES payloads and Annex-B NAL units and feed
             * them individually to the h264bsd decoder. This version handles parameter sets
             * more carefully to ensure proper decoder initialization.
             */
            const uint8_t *seg = data;
            // const uint8_t *seg_end = data + size; // unused

            // Quick check for TS sync byte at offset 0 (0x47). If not present, skip TS demux.
            if (size >= 188 && seg[0] == 0x47) {
                // Lightweight TS -> PES assembly for a single video PID
                int video_pid = -1;
                uint8_t *pes_buf = NULL;
                size_t pes_buf_len = 0;

                // Find plausible TS sync offset (support segments that might include extra data)
                int sync_offset = -1;
                for (int off = 0; off < 188; ++off) {
                    if (off + 188 * 3 > (int)size) break;
                    if (seg[off] == 0x47 && seg[off + 188] == 0x47 && seg[off + 376] == 0x47) { sync_offset = off; break; }
                }
                if (sync_offset < 0) sync_offset = 0;

                for (size_t pos = sync_offset; pos + 188 <= size; pos += 188) {
                    const uint8_t *pkt = seg + pos;
                    if (pkt[0] != 0x47) continue; // invalid/shifted
                    uint8_t payload_unit_start = (pkt[1] & 0x40) != 0;
                    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
                    uint8_t afc = (pkt[3] & 0x30) >> 4; // adaptation_field_control
                    size_t payload_offset = 4;
                    if (afc == 2) continue; // adaptation only
                    if (afc == 3) {
                        // adaptation field present
                        uint8_t af_len = pkt[4];
                        payload_offset = 5 + af_len;
                        if (payload_offset > 188) continue;
                    }
                    if (payload_offset >= 188) continue;
                    const uint8_t *payload = pkt + payload_offset;
                    size_t payload_len = 188 - payload_offset;

                    if (payload_len == 0) continue;

                    // If we haven't chosen a video PID yet, detect PES start with stream_id 0xE0
                    if (video_pid == -1 && payload_unit_start && payload_len >= 6) {
                        if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01 && (payload[3] & 0xF0) == 0xE0) {
                            video_pid = pid;
                        }
                    }

                    if (video_pid != -1 && pid == video_pid) {
                        // If start of PES, flush previous PES buffer first
                        if (payload_unit_start && pes_buf_len > 0) {
                            if (!pes_dump_done) {
                                FILE *df = fopen("/tmp/anhelo_pes_dump.bin", "wb");
                                if (df) {
                                    fwrite(pes_buf, 1, pes_buf_len, df);
                                    fclose(df);
                                    printf("[DEBUG] dumped pes buf len=%zu to /tmp/anhelo_pes_dump.bin\n", pes_buf_len);
                                    fflush(stdout);
                                } else {
                                    printf("[DEBUG] failed to open dump file for PES\n"); fflush(stdout);
                                }
                                pes_dump_done = 1;
                            }
                            // Try feeding the entire assembled PES payload to the decoder in one go
                            {
                                simple_h264_frame_t frame_all = {0};
                                simple_h264_result_t result_all = simple_h264_decode(h264_decoder, pes_buf, pes_buf_len, &frame_all);
                                printf("[DEBUG] TS->PES whole feed len=%zu result=%s pic=%p w=%u h=%u\n",
                                       pes_buf_len, simple_h264_result_string(result_all), (void*)frame_all.y_plane, frame_all.width, frame_all.height);
                                
                                // Provide status details for whole PES processing  
                                if (result_all == SIMPLE_H264_ERROR) {
                                    printf("[DEBUG] TS->PES whole feed caused decoder error\n");
                                } else if (result_all == SIMPLE_H264_PARAM_SET_ERROR) {
                                    printf("[DEBUG] TS->PES whole feed parameter set error\n");
                                } else if (result_all == SIMPLE_H264_HEADERS_READY) {
                                    printf("[DEBUG] TS->PES whole feed headers ready\n");
                                }
                                fflush(stdout);
                                
                                if (result_all == SIMPLE_H264_FRAME_READY && frame_all.y_plane && frame_all.width > 0 && frame_all.height > 0) {
                                    if (!video) { if (init_video_output(frame_all.width, frame_all.height) < 0) { /* ignore */ } }
                                    static int last_w_all = 0, last_h_all = 0;
                                    if (!rgb_buffer || last_w_all != (int)frame_all.width || last_h_all != (int)frame_all.height) {
                                        free(rgb_buffer);
                                        rgb_buffer = (uint8_t*)malloc(frame_all.width * frame_all.height * 3);
                                        last_w_all = (int)frame_all.width; last_h_all = (int)frame_all.height;
                                    }
                                    if (rgb_buffer) {
                                        yuv420_to_rgb24((int)frame_all.width, (int)frame_all.height, 
                                                       frame_all.y_plane, frame_all.u_plane, frame_all.v_plane,
                                                       (int)frame_all.y_stride, (int)frame_all.uv_stride, (int)frame_all.uv_stride, 
                                                       rgb_buffer, (int)frame_all.width * 3);
                                        video_draw(video, rgb_buffer, (int)frame_all.width * 3);
                                        frames_displayed++;
                                        uint64_t now = get_time_us();
                                        if (now - last_frame_time < frame_duration_us) usleep(frame_duration_us - (now - last_frame_time));
                                        last_frame_time = get_time_us();
                                        if (video && video_poll(video)) { should_quit_hls = 1; return 1; }
                                    }
                                }
                            }
                            // The PES payload may contain Annex-B start codes (0x000001/0x00000001)
                            // or length-prefixed NALs (common in some packagers). Detect which
                            // format is present and parse appropriately.
                            const uint8_t *b = pes_buf;
                            const uint8_t *e = pes_buf + pes_buf_len;

                            // Quick scan for Annex-B start codes
                            int has_start_codes = 0;
                            for (const uint8_t *p = b; p + 3 < e; ++p) {
                                if (p[0] == 0 && p[1] == 0 && p[2] == 1) { has_start_codes = 1; break; }
                            }

                            if (has_start_codes) {
                                // Enhanced Annex-B parsing with parameter set prioritization
                                const uint8_t *s = b;
                                
                                // First pass: look for and process parameter sets (SPS/PPS)
                                s = b;
                                while (s + 3 < e) {
                                    // find start code
                                    const uint8_t *sc = NULL;
                                    const uint8_t *p = s;
                                    for (; p + 3 < e; ++p) {
                                        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { sc = p; break; }
                                        if (p + 4 < e && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { sc = p; break; }
                                    }
                                    if (!sc) break;
                                    int sc_len = (sc[2] == 1) ? 3 : 4;
                                    const uint8_t *nal_start = sc + sc_len;
                                    const uint8_t *next = nal_start;
                                    for (; next + 3 < e; ++next) {
                                        if (next[0] == 0 && next[1] == 0 && next[2] == 1) break;
                                        if (next + 4 < e && next[0] == 0 && next[1] == 0 && next[2] == 0 && next[3] == 1) break;
                                    }
                                    size_t nal_len = (size_t)(next - nal_start);
                                    if (nal_len > 0) {
                                        int nal_type = get_nal_unit_type(nal_start, nal_len);
                                        // Process parameter sets first
                                        if (is_parameter_set(nal_type)) {
                                            process_h264_nal_unit(nal_start, nal_len, "TS->PES Annex-B ParamSet");
                                        }
                                    }
                                    s = next;
                                }
                                
                                // Second pass: process slices and other NAL units
                                s = b;
                                while (s + 3 < e) {
                                    // find start code
                                    const uint8_t *sc = NULL;
                                    const uint8_t *p = s;
                                    for (; p + 3 < e; ++p) {
                                        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { sc = p; break; }
                                        if (p + 4 < e && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { sc = p; break; }
                                    }
                                    if (!sc) break;
                                    int sc_len = (sc[2] == 1) ? 3 : 4;
                                    const uint8_t *nal_start = sc + sc_len;
                                    const uint8_t *next = nal_start;
                                    for (; next + 3 < e; ++next) {
                                        if (next[0] == 0 && next[1] == 0 && next[2] == 1) break;
                                        if (next + 4 < e && next[0] == 0 && next[1] == 0 && next[2] == 0 && next[3] == 1) break;
                                    }
                                    size_t nal_len = (size_t)(next - nal_start);
                                    if (nal_len > 0) {
                                        int nal_type = get_nal_unit_type(nal_start, nal_len);
                                        // Skip parameter sets (already processed) and process others
                                        if (!is_parameter_set(nal_type)) {
                                            if (process_h264_nal_unit(nal_start, nal_len, "TS->PES Annex-B NAL")) {
                                                if (video && video_poll(video)) { should_quit_hls = 1; return 1; }
                                            }
                                        }
                                    }
                                    s = next;
                                }
                            } else {
                                // No start codes found: try common length-prefixed format (4-byte NAL size, big-endian)
                                // Enhanced length-prefixed parsing with parameter set prioritization
                                const uint8_t *p = b;
                                
                                // First pass: process parameter sets
                                p = b;
                                while (p + 4 <= e) {
                                    uint32_t nal_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
                                    p += 4;
                                    if (nal_len == 0 || p + nal_len > e) break;
                                    int nal_type = get_nal_unit_type(p, nal_len);
                                    if (is_parameter_set(nal_type)) {
                                        process_h264_nal_unit(p, nal_len, "TS->PES LenPref ParamSet");
                                    }
                                    p += nal_len;
                                }
                                
                                // Second pass: process other NAL units
                                p = b;
                                while (p + 4 <= e) {
                                    uint32_t nal_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
                                    p += 4;
                                    if (nal_len == 0 || p + nal_len > e) break;
                                    int nal_type = get_nal_unit_type(p, nal_len);
                                    if (!is_parameter_set(nal_type)) {
                                        if (process_h264_nal_unit(p, nal_len, "TS->PES LenPref NAL")) {
                                            if (video && video_poll(video)) { should_quit_hls = 1; return 1; }
                                        }
                                    }
                                    p += nal_len;
                                }
                            }

                            free(pes_buf); pes_buf = NULL; pes_buf_len = 0;
                        }

                        // If this payload contains PES start, skip PES header and append payload body
                        if (payload_unit_start && payload_len >= 9 && payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
                            uint8_t pes_header_data_len = 0;
                            if (payload_len >= 9) pes_header_data_len = payload[8];
                            size_t data_start = 9 + pes_header_data_len;
                            if (data_start <= payload_len) {
                                size_t add_len = payload_len - data_start;
                                uint8_t *nb = realloc(pes_buf, pes_buf_len + add_len);
                                if (!nb) { free(pes_buf); pes_buf = NULL; pes_buf_len = 0; break; }
                                pes_buf = nb;
                                memcpy(pes_buf + pes_buf_len, payload + data_start, add_len);
                                pes_buf_len += add_len;
                            }
                        } else {
                            // append whole payload
                            uint8_t *nb = realloc(pes_buf, pes_buf_len + payload_len);
                            if (!nb) { free(pes_buf); pes_buf = NULL; pes_buf_len = 0; break; }
                            pes_buf = nb;
                            memcpy(pes_buf + pes_buf_len, payload, payload_len);
                            pes_buf_len += payload_len;
                        }
                    }
                }

                // Final flush of any remaining PES buffer
                if (pes_buf_len > 0 && pes_buf) {
                    if (!pes_dump_done) {
                        FILE *df = fopen("/tmp/anhelo_pes_dump.bin", "wb");
                        if (df) { fwrite(pes_buf, 1, pes_buf_len, df); fclose(df); printf("[DEBUG] dumped final pes buf len=%zu to /tmp/anhelo_pes_dump.bin\n", pes_buf_len); fflush(stdout); }
                        pes_dump_done = 1;
                    }
                    const uint8_t *b = pes_buf;
                    const uint8_t *e = pes_buf + pes_buf_len;

                    int has_start_codes = 0;
                    for (const uint8_t *p = b; p + 3 < e; ++p) {
                        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { has_start_codes = 1; break; }
                    }

                    if (has_start_codes) {
                        const uint8_t *s = b;
                        
                        // Process parameter sets first
                        s = b;
                        while (s + 3 < e) {
                            const uint8_t *sc = NULL;
                            const uint8_t *p = s;
                            for (; p + 3 < e; ++p) {
                                if (p[0] == 0 && p[1] == 0 && p[2] == 1) { sc = p; break; }
                                if (p + 4 < e && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { sc = p; break; }
                            }
                            if (!sc) break;
                            int sc_len = (sc[2] == 1) ? 3 : 4;
                            const uint8_t *nal_start = sc + sc_len;
                            const uint8_t *next = nal_start;
                            for (; next + 3 < e; ++next) {
                                if (next[0] == 0 && next[1] == 0 && next[2] == 1) break;
                                if (next + 4 < e && next[0] == 0 && next[1] == 0 && next[2] == 0 && next[3] == 1) break;
                            }
                            size_t nal_len = (size_t)(next - nal_start);
                            if (nal_len > 0) {
                                int nal_type = get_nal_unit_type(nal_start, nal_len);
                                if (is_parameter_set(nal_type)) {
                                    process_h264_nal_unit(nal_start, nal_len, "Final TS flush ParamSet");
                                }
                            }
                            s = next;
                        }
                        
                        // Process other NAL units
                        s = b;
                        while (s + 3 < e) {
                            const uint8_t *sc = NULL;
                            const uint8_t *p = s;
                            for (; p + 3 < e; ++p) {
                                if (p[0] == 0 && p[1] == 0 && p[2] == 1) { sc = p; break; }
                                if (p + 4 < e && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { sc = p; break; }
                            }
                            if (!sc) break;
                            int sc_len = (sc[2] == 1) ? 3 : 4;
                            const uint8_t *nal_start = sc + sc_len;
                            const uint8_t *next = nal_start;
                            for (; next + 3 < e; ++next) {
                                if (next[0] == 0 && next[1] == 0 && next[2] == 1) break;
                                if (next + 4 < e && next[0] == 0 && next[1] == 0 && next[2] == 0 && next[3] == 1) break;
                            }
                            size_t nal_len = (size_t)(next - nal_start);
                            if (nal_len > 0) {
                                int nal_type = get_nal_unit_type(nal_start, nal_len);
                                if (!is_parameter_set(nal_type)) {
                                    process_h264_nal_unit(nal_start, nal_len, "Final TS flush NAL");
                                }
                            }
                            s = next;
                        }
                    }
                    free(pes_buf);
                    pes_buf = NULL; pes_buf_len = 0;
                }
            } else {
                // Not a TS segment; keep previous simple Annex-B fallback (search for start codes)
                const uint8_t *buf = data;
                const uint8_t *end = data + size;
                const uint8_t *pos = buf;
                
                // First pass: look for and process parameter sets
                pos = buf;
                while (pos + 3 < end) {
                    const uint8_t *start = NULL;
                    const uint8_t *p = pos;
                    for (; p + 3 < end; ++p) {
                        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { start = p + 3; break; }
                        if (p + 4 < end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { start = p + 4; break; }
                    }
                    if (!start) break;
                    const uint8_t *next = start;
                    for (; next + 3 < end; ++next) {
                        if (next[0] == 0 && next[1] == 0 && next[2] == 1) break;
                        if (next + 4 < end && next[0] == 0 && next[1] == 0 && next[2] == 0 && next[3] == 1) break;
                    }
                    size_t nal_len = (size_t)(next - start);
                    if (nal_len > 0) {
                        int nal_type = get_nal_unit_type(start, nal_len);
                        if (is_parameter_set(nal_type)) {
                            process_h264_nal_unit(start, nal_len, "Fallback ParamSet");
                        }
                    }
                    pos = next;
                }
                
                // Second pass: process other NAL units  
                pos = buf;
                while (pos + 3 < end) {
                    const uint8_t *start = NULL;
                    const uint8_t *p = pos;
                    for (; p + 3 < end; ++p) {
                        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { start = p + 3; break; }
                        if (p + 4 < end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { start = p + 4; break; }
                    }
                    if (!start) break;
                    const uint8_t *next = start;
                    for (; next + 3 < end; ++next) {
                        if (next[0] == 0 && next[1] == 0 && next[2] == 1) break;
                        if (next + 4 < end && next[0] == 0 && next[1] == 0 && next[2] == 0 && next[3] == 1) break;
                    }
                    size_t nal_len = (size_t)(next - start);
                    if (nal_len > 0) {
                        int nal_type = get_nal_unit_type(start, nal_len);
                        if (!is_parameter_set(nal_type)) {
                            if (process_h264_nal_unit(start, nal_len, "Fallback NAL")) {
                                if (video && video_poll(video)) { should_quit_hls = 1; return 1; }
                                break; // we found a picture and displayed it
                            }
                        }
                    }
                    pos = next;
                }
            }
        }
    } else if (use_custom_decoder == 2) {
        // Use MPEG-4 decoder
#ifdef USE_MPEG4
        uint8_t *y_plane, *u_plane, *v_plane;
        int stride_y, stride_uv;
        
        mpeg4_error_t err = mpeg4_decode_frame(mpeg4_decoder, data, size, 
                                              &y_plane, &u_plane, &v_plane, 
                                              &stride_y, &stride_uv);
        
        if (err == MPEG4_SUCCESS) {
            int width, height;
            mpeg4_get_frame_size(mpeg4_decoder, &width, &height);
            
            if (!video) {
                if (init_video_output(width, height) < 0) {
                    return -1;
                }
            }
            
            // Allocate/resize RGB buffer
            static int hls_mpeg_last_w = 0, hls_mpeg_last_h = 0;
            if (!rgb_buffer || hls_mpeg_last_w != width || hls_mpeg_last_h != height) {
                free(rgb_buffer);
                rgb_buffer = (uint8_t *)malloc(width * height * 3);
                if (!rgb_buffer) { fprintf(stderr, "Failed to allocate RGB buffer (HLS MPEG-4)\n"); return -1; }
                hls_mpeg_last_w = width; hls_mpeg_last_h = height;
            }

            // Convert and draw (assumes 4:2:0 planes from decoder)
            yuv420_to_rgb24(width, height, y_plane, u_plane, v_plane, stride_y, stride_uv, stride_uv,
                            rgb_buffer, width * 3);

            video_draw(video, rgb_buffer, width * 3);
            frames_displayed++;
            
            // Handle timing
            uint64_t current_time = get_time_us();
            if (current_time - last_frame_time < frame_duration_us) {
                usleep(frame_duration_us - (current_time - last_frame_time));
            }
            last_frame_time = get_time_us();
        }
#endif
    }
    
    return 0;
}

void cleanup_resources() {
#ifndef NO_FFMPEG
    if (rgb_buffer) {
        av_free(rgb_buffer);
        rgb_buffer = NULL;
    }
    if (rgb_frame) {
        av_frame_free(&rgb_frame);
    }
    if (frame) {
        av_frame_free(&frame);
    }
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    if (format_ctx) {
        avformat_close_input(&format_ctx);
    }
#endif

#ifdef NO_FFMPEG
    if (rgb_buffer) {
        free(rgb_buffer);
        rgb_buffer = NULL;
    }
    // Custom scaling context cleanup would go here
#endif
    
    // Clean up custom decoders
    if (h264_decoder) {
        simple_h264_destroy(h264_decoder);
        h264_decoder = NULL;
    }
#ifdef USE_MPEG4
    if (mpeg4_decoder) {
        mpeg4_destroy_decoder(mpeg4_decoder);
        mpeg4_decoder = NULL;
    }
#endif
    
    // Clean up HLS demuxer
    if (hls_demuxer) {
        hls_demuxer_destroy(hls_demuxer);
        hls_demuxer = NULL;
    }
    if (video) {
        video_destroy(video);
        video = NULL;
    }
    if (screen) {
        SDL_FreeSurface(screen);
        screen = NULL;
    }
    
    // Clean up memory pools
    if (frame_pool) {
        frame_pool_destroy(frame_pool);
        frame_pool = NULL;
    }
    if (string_pool) {
        pool_destroy(string_pool);
        string_pool = NULL;
    }
    
    SDL_Quit();
}

int get_user_input_gui(char *buffer, size_t buffer_size) {
    SDL_Event event;
    size_t pos = 0;
    int done = 0;
    
    // Clear the screen
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
    SDL_Flip(screen);
    
    printf("Enter Twitch channel or stream URL (GUI mode - type and press Enter): ");
    fflush(stdout);
    
    while (!done) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    return -1; // User wants to quit
                    
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        return -1; // User wants to quit
                    } else if (event.key.keysym.sym == SDLK_RETURN || 
                               event.key.keysym.sym == SDLK_KP_ENTER) {
                        buffer[pos] = '\0';
                        done = 1;
                    } else if (event.key.keysym.sym == SDLK_BACKSPACE && pos > 0) {
                        pos--;
                        buffer[pos] = '\0';
                        printf("\b \b");
                        fflush(stdout);
                    } else if (event.key.keysym.unicode && 
                               event.key.keysym.unicode < 128 &&
                               event.key.keysym.unicode >= 32 &&
                               pos < buffer_size - 1) {
                        buffer[pos] = (char)event.key.keysym.unicode;
                        printf("%c", buffer[pos]);
                        fflush(stdout);
                        pos++;
                    }
                    break;
            }
        }
        SDL_Delay(10);
    }
    
    printf("\n");
    return pos > 0 ? 0 : -1;
}

int get_user_input_terminal(char *buffer, size_t buffer_size) {
    printf("Enter Twitch channel or stream URL: ");
    fflush(stdout);
    
    if (fgets(buffer, buffer_size, stdin) == NULL) {
        return -1;
    }
    
    // Remove trailing newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }
    
    return strlen(buffer) > 0 ? 0 : -1;
}

int is_hls_stream(const char *url) {
    if (!url) return 0;
    const char *q = strchr(url, '?');
    size_t len = strlen(url);
    if (q) len = (size_t)(q - url);
    // Look for ".m3u8" before query/hash
    const char *p = url;
    while ((p = strstr(p, ".m3u8")) != NULL) {
        if ((size_t)(p - url) < len) return 1;
        p += 5; // advance past match
    }
    return 0;
}

char *resolve_stream_url(const char *input) {
    if (!input || strlen(input) == 0) {
        return NULL;
    }
    
    // Check if it's already a direct URL (contains http:// or https://)
    if (strstr(input, "http://") == input || strstr(input, "https://") == input) {
        return strdup(input);
    }
    
    // Check if it's an HLS stream
    if (is_hls_stream(input)) {
        return strdup(input);
    }
    
    // Try to resolve as Twitch channel
    printf("Resolving Twitch channel: %s\n", input);
    char *resolved = twitch_resolve(input);
    if (resolved) {
        printf("Resolved to: %s\n", resolved);
        return resolved;
    }
    
    printf("Failed to resolve Twitch channel, trying as direct URL...\n");
    return strdup(input);
}

#ifndef NO_FFMPEG
int init_ffmpeg(const char *url) {
    // Initialize FFmpeg (not needed in newer versions)
    avformat_network_init();
    
    // Open input file/stream
    format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        fprintf(stderr, "Failed to allocate format context\n");
        return -1;
    }
    
    // Set options for network streams - optimized for memory efficiency
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "timeout", "10000000", 0); // 10 second timeout
    av_dict_set(&opts, "user_agent", "anhelo/1.0", 0);
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Minimal buffer settings for low memory environments
    int optimal_buffer = 512 * 1024; // 512KB for very low memory
    int probe_size = 8192; // Smaller probe size
    int analyze_duration = 500000; // 0.5 second analysis
#else
    // Dynamic buffer sizing based on estimated bitrate (default to 4MB)
    int optimal_buffer = 4 * 1024 * 1024; // 4MB default
    int probe_size = 32768; // Larger probe for better detection
    int analyze_duration = 2000000; // 2 second analysis
#endif

    char buffer_size_str[32];
    snprintf(buffer_size_str, sizeof(buffer_size_str), "%d", optimal_buffer);
    av_dict_set(&opts, "buffer_size", buffer_size_str, 0);
    
    char probe_size_str[32];
    snprintf(probe_size_str, sizeof(probe_size_str), "%d", probe_size);
    av_dict_set(&opts, "probesize", probe_size_str, 0);
    
    char analyze_duration_str[32];
    snprintf(analyze_duration_str, sizeof(analyze_duration_str), "%d", analyze_duration);
    av_dict_set(&opts, "analyzeduration", analyze_duration_str, 0);
    
    av_dict_set(&opts, "max_delay", "500000", 0); // 0.5 second max delay
    av_dict_set(&opts, "fflags", "+genpts+discardcorrupt", 0); // Handle corrupt data better
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Memory-optimized settings
    av_dict_set(&opts, "rtbufsize", "1048576", 0); // 1MB real-time buffer
    av_dict_set(&opts, "hls_list_size", "3", 0); // Keep fewer segments
    av_dict_set(&opts, "hls_flags", "delete_segments", 0); // Delete segments when done
#else
    av_dict_set(&opts, "rtbufsize", "16777216", 0); // 16MB real-time buffer
    av_dict_set(&opts, "hls_list_size", "10", 0); // Keep more segments
#endif
    
    if (avformat_open_input(&format_ctx, url, NULL, &opts) < 0) {
        fprintf(stderr, "Failed to open input: %s\n", url);
        av_dict_free(&opts);
        return -1;
    }
    av_dict_free(&opts);
    
    // Find stream information
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to find stream information\n");
        return -1;
    }
    
    // Find video stream
    int video_stream_idx = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    if (video_stream_idx == -1) {
        fprintf(stderr, "No video stream found\n");
        return -1;
    }
    
    // Calculate frame rate from the stream
    AVStream *video_stream = format_ctx->streams[video_stream_idx];
    if (video_stream->avg_frame_rate.den != 0) {
        frame_rate = (double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    } else if (video_stream->r_frame_rate.den != 0) {
        frame_rate = (double)video_stream->r_frame_rate.num / video_stream->r_frame_rate.den;
    } else {
        frame_rate = 30.0; // Fallback to 30 FPS
    }
    
    // Ensure reasonable frame rate bounds
    if (frame_rate <= 0 || frame_rate > 120) {
        frame_rate = 30.0;
    }
    
    frame_duration_us = (uint64_t)(1000000.0 / frame_rate);
    printf("Detected frame rate: %.2f FPS (frame duration: %lu us)\n", frame_rate, frame_duration_us);
    
    // Get codec parameters
    AVCodecParameters *codecpar = format_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        return -1;
    }
    
    // Optimize codec context for memory usage
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        return -1;
    }
    
    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to context\n");
        return -1;
    }
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Use fewer threads for decoding in low-memory mode
    codec_ctx->thread_count = 1; // Single thread for memory savings
    codec_ctx->thread_type = FF_THREAD_SLICE; // Slice threading uses less memory
    codec_ctx->lowres = 1; // Lower resolution decoding (if codec supports it)
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    
    // Set additional codec options for memory savings
    AVDictionary *codec_opts = NULL;
    av_dict_set(&codec_opts, "threads", "1", 0);
    // Enable refcounted_frames to avoid missing-reference errors when packets/frames are skipped
    av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
#else
    // Enable multi-threading for better performance
    codec_ctx->thread_count = 0; // Auto-detect thread count
    codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    
    // Optimize for streaming with better buffering
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    
    // Set additional codec options for smooth streaming
    AVDictionary *codec_opts = NULL;
    av_dict_set(&codec_opts, "threads", "auto", 0);
    av_dict_set(&codec_opts, "thread_type", "frame+slice", 0);
#endif
    
    av_dict_set(&codec_opts, "tune", "fastdecode", 0); // Optimize for decoding speed
    av_dict_set(&codec_opts, "preset", "fast", 0); // Balance between speed and quality
    
    // Open codec
    if (avcodec_open2(codec_ctx, codec, &codec_opts) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        av_dict_free(&codec_opts);
        return -1;
    }
    av_dict_free(&codec_opts);
    
    // Allocate frames
    frame = av_frame_alloc();
    rgb_frame = av_frame_alloc();
    if (!frame || !rgb_frame) {
        fprintf(stderr, "Failed to allocate frames\n");
        return -1;
    }
    
    // Allocate buffer for RGB frame with memory-efficient alignment
#ifdef MINIMAL_MEMORY_BUFFERS
    // In low-memory mode, use minimal alignment for memory savings
    int rgb_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codec_ctx->width, codec_ctx->height, 1);
#else
    // Optimized alignment for performance
    int rgb_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codec_ctx->width, codec_ctx->height, 16);
#endif
    
    rgb_buffer = (uint8_t *)av_malloc(rgb_buffer_size);
    if (!rgb_buffer) {
        fprintf(stderr, "Failed to allocate RGB buffer\n");
        return -1;
    }
    
#ifdef MINIMAL_MEMORY_BUFFERS
    if (av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer, AV_PIX_FMT_RGB24, 
                            codec_ctx->width, codec_ctx->height, 1) < 0) {
#else
    if (av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer, AV_PIX_FMT_RGB24, 
                            codec_ctx->width, codec_ctx->height, 16) < 0) {
#endif
        fprintf(stderr, "Failed to fill RGB frame arrays\n");
        return -1;
    }
    
    // Initialize scaling context with optimized flags
    {
        int sws_flags = SWS_FAST_BILINEAR;
#ifdef MINIMAL_MEMORY_BUFFERS
        // lowest-memory/resolution-preserving scaling
        sws_flags = SWS_POINT;
#endif
        sws_ctx = sws_getCachedContext(NULL,
                                      codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                      codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGB24,
                                      sws_flags, NULL, NULL, NULL);
        if (!sws_ctx) {
            fprintf(stderr, "Failed to initialize scaling context (sws)\n");
            return -1;
        }
    }
    
    printf("Video stream: %dx%d, codec: %s\n", 
           codec_ctx->width, codec_ctx->height, codec->name);
    
    return video_stream_idx;
}
#endif

// H264BSD decoder functions removed - using simple_h264 decoder instead

int init_video_output(int width, int height) {
    // Create video output
    video = video_create(width, height);
    if (!video) {
        fprintf(stderr, "Failed to create video output\n");
        return -1;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    char input_buffer[512];
    char *stream_url = NULL;
#ifndef NO_FFMPEG
    int video_stream_idx = -1;
    AVPacket packet; /* stack packet - avoid holding packet memory between iterations */
    int should_quit = 0;
#endif
    /* cURL easy handles are initialized per-use in resolver/demuxer. */
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        fprintf(stderr, "Falling back to terminal mode\n");
        screen = NULL;
    } else {
        // Enable Unicode for text input
        SDL_EnableUNICODE(1);
        SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
        
#ifdef BACKEND_OPENGL
        // Set OpenGL attributes
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        
        // Create OpenGL window
        screen = SDL_SetVideoMode(640, 480, 32, SDL_OPENGL);
#else
        // Create software surface window
        screen = SDL_SetVideoMode(640, 480, 24, SDL_SWSURFACE);
#endif
        
        if (!screen) {
            fprintf(stderr, "Failed to create SDL surface: %s\n", SDL_GetError());
            fprintf(stderr, "Falling back to terminal mode\n");
            SDL_Quit();
            screen = NULL;
        }
        
        if (screen) {
            SDL_WM_SetCaption("Anhelo - Video Stream Player", "Anhelo");
        }
    }
    
    // Get input from user
    if (argc > 1) {
        strncpy(input_buffer, argv[1], sizeof(input_buffer) - 1);
        input_buffer[sizeof(input_buffer) - 1] = '\0';
    } else {
        int input_result;
        if (screen) {
            input_result = get_user_input_gui(input_buffer, sizeof(input_buffer));
        } else {
            input_result = get_user_input_terminal(input_buffer, sizeof(input_buffer));
        }
        
        if (input_result < 0) {
            printf("No input provided or user quit\n");
            cleanup_resources();
            return 1;
        }
    }
    
    printf("Input: %s\n", input_buffer);
    
    // Resolve stream URL
    stream_url = resolve_stream_url(input_buffer);
    if (!stream_url) {
        fprintf(stderr, "Failed to resolve stream URL\n");
        cleanup_resources();
        return 1;
    }
    
    printf("Stream URL: %s\n", stream_url);
    
    // Check if we should use HLS demuxer
    use_hls_demuxer = is_hls_stream(stream_url);
    
#ifdef NO_FFMPEG
    // When NO_FFMPEG is defined, only HLS streams with custom decoders are supported
    if (!use_hls_demuxer) {
        fprintf(stderr, "NO_FFMPEG mode requires HLS streams (.m3u8 URLs)\n");
        free(stream_url);
        cleanup_resources();
        return 1;
    }
#endif
    
    if (use_hls_demuxer
#ifdef NO_FFMPEG
    && 1
#else
    && 0 /* Prefer FFmpeg for HLS when available */
#endif
    ) {
        // Use HLS demuxer
        hls_demuxer = hls_demuxer_create();
        if (!hls_demuxer) {
            fprintf(stderr, "Failed to create HLS demuxer\n");
            free(stream_url);
            cleanup_resources();
            return 1;
        }
        
        // Determine decoder to use based on URL or content
        // For now, assume H.264 for HLS streams
        use_custom_decoder = 1;
        
        // Initialize custom decoder
        if (use_custom_decoder == 1) {
            h264_decoder = simple_h264_create();
            if (!h264_decoder) {
                fprintf(stderr, "Failed to initialize simple H.264 decoder\n");
                free(stream_url);
                cleanup_resources();
                return 1;
            }
        } else if (use_custom_decoder == 2) {
#ifdef USE_MPEG4
            // MPEG-4 decoder initialization would go here
            // For now, assume 640x480
            mpeg4_decoder = mpeg4_create_decoder(640, 480);
            if (!mpeg4_decoder) {
                fprintf(stderr, "Failed to create MPEG-4 decoder\n");
                free(stream_url);
                cleanup_resources();
                return 1;
            }
#endif
        }
        
    // Initialize frames and scaling for custom decoders
#ifndef NO_FFMPEG
        frame = av_frame_alloc();
        rgb_frame = av_frame_alloc();
        if (!frame || !rgb_frame) {
            fprintf(stderr, "Failed to allocate frames\n");
            free(stream_url);
            cleanup_resources();
            return 1;
        }
#else
    // No FFmpeg: RGB buffer allocated lazily in callback
#endif
        
        printf("Starting HLS playback... Press Q or ESC to quit\n");
        
        // Process HLS stream
    should_quit_hls = 0;
    hls_error_t hls_err = hls_process_stream(hls_demuxer, stream_url, hls_segment_callback, NULL);
        if (hls_err != HLS_OK) {
            fprintf(stderr, "HLS processing failed: %s\n", hls_get_error_string(hls_err));
        }
        
        hls_demuxer_destroy(hls_demuxer);
        if (should_quit_hls) {
            printf("Playback interrupted by user\n");
        } else if (hls_err != HLS_OK) {
            fprintf(stderr, "HLS processing failed: %s\n", hls_get_error_string(hls_err));
        }
    } else {
#ifndef NO_FFMPEG
        // Use FFmpeg for non-HLS streams
        video_stream_idx = init_ffmpeg(stream_url);
        if (video_stream_idx < 0) {
            fprintf(stderr, "Failed to initialize FFmpeg\n");
            free(stream_url);
            cleanup_resources();
            return 1;
        }
        
        // Initialize video output
        if (init_video_output(codec_ctx->width, codec_ctx->height) < 0) {
            fprintf(stderr, "Failed to initialize video output\n");
            free(stream_url);
            cleanup_resources();
            return 1;
        }
        
        printf("Starting playback... Press Q or ESC to quit\n");
        
        // Initialize stack packet (no long-lived allocation)
        memset(&packet, 0, sizeof(packet));
        
        // Initialize timing
        last_frame_time = get_time_us();
        
        // Main playback loop with proper frame timing
        while (!should_quit && av_read_frame(format_ctx, &packet) >= 0) {
            if (packet.stream_index == video_stream_idx) {
                /* Avoid pre-decode packet dropping: it breaks decoder references
                 * (missing refs, mmco failures). We do frameskip post-decode only.
                 */
                // Send packet to decoder
                int ret = avcodec_send_packet(codec_ctx, &packet);
                if (ret < 0) {
                    if (ret == AVERROR_EOF) {
                        break;
                    }
                    // Skip problematic packets instead of failing completely
                    av_packet_unref(&packet);
                    continue;
                }
                
                // Receive decoded frame(s)
                while (ret >= 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    } else if (ret < 0) {
                        fprintf(stderr, "Error receiving frame from decoder: %d\n", ret);
                        break;
                    }
                    
                    // Improved frame timing control for smoother playback
                    uint64_t current_time = get_time_us();
                    uint64_t elapsed = current_time - last_frame_time;
                    
    #ifndef DISABLE_FRAMESKIP
                    /* Compile-time frameskip behavior:
                     * After displaying a frame we set skip_remaining = FRAMESKIP_AMOUNT.
                     * While skip_remaining > 0 we skip decoded frames (counting them as dropped)
                     * and decrement the counter. This yields: [displayed, skip...skip, displayed].
                     */
                    if (FRAMESKIP_AMOUNT > 0 && skip_remaining > 0) {
                        // Discard this decoded frame and advance the playback clock
                        // so we don't try to "catch up" by showing skipped frames.
                        frames_dropped++;
                        skip_remaining--;
                        last_frame_time += frame_duration_us; // advance timeline for skipped frame
                        continue;
                    }
    #endif
                    
                    // Smoother timing with better sleep handling
                    if (elapsed < frame_duration_us) {
                        uint64_t sleep_time = frame_duration_us - elapsed;
                        if (sleep_time > 1000 && sleep_time < 50000) { // Sleep between 1ms and 50ms
                            usleep(sleep_time);
                        }
                    }
                    
                    // Update timing
                    last_frame_time = get_time_us();
                    
                    // Ensure scaler/buffers match current frame size/format (HLS ads/resolution switches)
                    {
                        static int sws_w = 0, sws_h = 0, sws_fmt = -1;
                        int f_w = frame->width > 0 ? frame->width : codec_ctx->width;
                        int f_h = frame->height > 0 ? frame->height : codec_ctx->height;
                        int f_fmt = frame->format >= 0 ? frame->format : codec_ctx->pix_fmt;
                        if (!sws_ctx || sws_w != f_w || sws_h != f_h || sws_fmt != f_fmt || !rgb_buffer) {
                            // Recreate sws context
                            if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = NULL; }
                            int sws_flags = SWS_FAST_BILINEAR;
                            sws_ctx = sws_getContext(f_w, f_h, (enum AVPixelFormat)f_fmt,
                                                     f_w, f_h, AV_PIX_FMT_RGB24,
                                                     sws_flags, NULL, NULL, NULL);
                            if (!sws_ctx) {
                                fprintf(stderr, "Failed to (re)create sws context for %dx%d fmt %d\n", f_w, f_h, f_fmt);
                                break;
                            }
                            // Reallocate RGB buffer and fill arrays
                            if (rgb_buffer) { av_free(rgb_buffer); rgb_buffer = NULL; }
                            int rgb_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, f_w, f_h, 16);
                            rgb_buffer = (uint8_t *)av_malloc(rgb_buffer_size);
                            if (!rgb_buffer) {
                                fprintf(stderr, "Failed to allocate RGB buffer (FFmpeg path)\n");
                                break;
                            }
                            if (av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, rgb_buffer,
                                                     AV_PIX_FMT_RGB24, f_w, f_h, 16) < 0) {
                                fprintf(stderr, "Failed to fill RGB arrays (FFmpeg path)\n");
                                break;
                            }
                            sws_w = f_w; sws_h = f_h; sws_fmt = f_fmt;
                            // Resize video output if needed
                            if (video) { video_destroy(video); video = NULL; }
                            if (init_video_output(f_w, f_h) < 0) {
                                fprintf(stderr, "Failed to reinit video output to %dx%d\n", f_w, f_h);
                                break;
                            }
                        }
                    }

                    // Convert frame to RGB (optimized scaling)
    #ifdef MINIMAL_MEMORY_BUFFERS
                    /* Slice-based conversion to reduce peak memory working set.
                     * Processes small horizontal stripes instead of converting whole frame at once.
                     * Adjust slice_h for a trade-off between CPU overhead and peak memory.
                     */
                    const int slice_h = 32; /* small stripe height */
                    for (int y = 0; y < codec_ctx->height; y += slice_h) {
                        int h = codec_ctx->height - y;
                        if (h > slice_h) h = slice_h;
                        uint8_t *dst_ptr = rgb_frame->data[0] + y * rgb_frame->linesize[0];
                        uint8_t *dst_data[4] = { dst_ptr, NULL, NULL, NULL };
                        int dst_linesize[4] = { rgb_frame->linesize[0], 0, 0, 0 };
                        sws_scale(sws_ctx,
                                  (uint8_t const * const *)frame->data, frame->linesize,
                                  y, h,
                                  dst_data, dst_linesize);
                    }
    #else
                    sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
                              frame->linesize, 0, frame->height > 0 ? frame->height : codec_ctx->height,
                              rgb_frame->data, rgb_frame->linesize);
    #endif
                    
                    // Draw frame (use correct stride)
                    video_draw(video, rgb_buffer, rgb_frame->linesize[0]);
                    frames_displayed++;
                    // Poll for quit events
                    if (video_poll(video)) {
                        should_quit = 1;
                        break;
                    }
                    /* After displaying a frame, set skip_remaining so the next
                     * FRAMESKIP_AMOUNT decoded frames are skipped.
                     */
                    skip_remaining = FRAMESKIP_AMOUNT;
                    
                    // Poll for quit events less frequently to reduce overhead
                    if (frames_displayed % 5 == 0 && video_poll(video)) {
                        should_quit = 1;
                        break;
                    }
                }
            }
            
        /* free packet data immediately after use (or after continue above) */
        av_packet_unref(&packet);
        /* re-init packet for next av_read_frame */
        memset(&packet, 0, sizeof(packet));
             
             // No additional delays - timing is controlled above
        }
        
        // Flush decoder
        printf("Flushing decoder...\n");
        avcodec_send_packet(codec_ctx, NULL);
        while (1) {
            int ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                break;
            }
            if (ret < 0) {
                break;
            }
            // Skip processing remaining frames during flush to speed up exit
        }
#else
        // NO_FFMPEG is defined but we got a non-HLS stream
        fprintf(stderr, "NO_FFMPEG mode only supports HLS streams\n");
        free(stream_url);
        cleanup_resources();
        return 1;
#endif
    }    printf("Playback finished\n");
    if (frames_displayed > 0) {
        printf("Performance: %d frames displayed, %d frames dropped (%.1f%% drop rate)\n", 
               frames_displayed, frames_dropped, 
               (float)frames_dropped / (frames_displayed + frames_dropped) * 100.0f);
    }
    
    // Cleanup
    free(stream_url);
    cleanup_resources();
    /* no global curl cleanup needed */

    return 0;
}
