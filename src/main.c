#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL/SDL.h>

#include "../include/video.h"
#include "../include/twitch.h"
#include "../include/memory_pool.h"

#ifdef USE_H264BSD
#include "h264/h264bsd_decoder.h"
#endif

#ifdef BACKEND_OPENGL
extern video_t *video_create(int width, int height);
extern void video_draw(video_t *v, const uint8_t *rgb, int linesize);
extern int video_poll(video_t *v);
extern void video_destroy(video_t *v);
#endif

// Global variables for cleanup
static AVFormatContext *format_ctx = NULL;
static AVCodecContext *codec_ctx = NULL;
static struct SwsContext *sws_ctx = NULL;
static AVFrame *frame = NULL;
static AVFrame *rgb_frame = NULL;
static uint8_t *rgb_buffer = NULL;
static video_t *video = NULL;
static SDL_Surface *screen = NULL;

// Memory pools for optimization
static frame_pool_t *frame_pool = NULL;
static memory_pool_t *string_pool = NULL;

// Frame timing variables - optimized for smooth playbook
static double frame_rate = 30.0; // Default frame rate
static uint64_t last_frame_time = 0;
static uint64_t frame_duration_us = 33333; // ~30 FPS in microseconds
static uint64_t frame_drop_threshold = 100000; // Drop frames if we're >100ms behind (less aggressive)
static int frames_dropped = 0;
static int frames_displayed = 0;
static int consecutive_drops = 0; // Track consecutive drops to avoid spiral
static double avg_decode_time = 16666.0; // Running average decode time in microseconds
static int skip_remaining = 0; // Runtime counter: skip this many decoded frames after last displayed frame

// Utility function to get current time in microseconds
uint64_t get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Smart frame dropping with adaptive thresholds
#ifndef FRAMESKIP_AMOUNT
#define FRAMESKIP_AMOUNT 3
#endif

static int should_drop_frame(uint64_t current_time, int queue_size, double decode_time_us) {
    (void)queue_size;
    // Don't drop if we're keeping up well
    if (consecutive_drops > 5) {
        consecutive_drops = 0; // Reset to avoid spiral
        return 0;
    }
    
    // Consider decode queue size (simulated based on timing)
    int estimated_queue = (int)((current_time - last_frame_time) / frame_duration_us);
    if (estimated_queue > FRAMESKIP_AMOUNT) return 1; // Too many frames behind
    
    // Adaptive threshold based on decode performance
    uint64_t adaptive_threshold = frame_drop_threshold * (1.0 + decode_time_us / 16666.0);
    
    return (current_time - last_frame_time) > adaptive_threshold;
}

void cleanup_resources() {
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
    const char *ext = strrchr(url, '.');
    return ext && strcmp(ext, ".m3u8") == 0;
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

#ifdef USE_H264BSD
static int is_h264_file(const char *path) {
    if (!path) return 0;
    const char *ext = strrchr(path, '.');
    return ext && strcmp(ext, ".h264") == 0;
}

/* Parse buffer and feed NAL units to h264bsd decoder. On PIC_RDY, convert and display. */
static int decode_and_display_h264bsd(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len <= 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    if (!buf) { fclose(f); fprintf(stderr, "malloc fail\n"); return -1; }
    if (fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); fprintf(stderr, "read fail\n"); return -1; }
    fclose(f);

    storage_t *storage = h264bsdAlloc();
    if (!storage) { free(buf); fprintf(stderr, "h264bsdAlloc failed\n"); return -1; }
    if (h264bsdInit(storage, 1) != H264BSD_RDY) {
        h264bsdFree(storage);
        free(buf);
        fprintf(stderr, "h264bsdInit failed\n");
        return -1;
    }

    /* Prepare AVFrame wrappers for sws conversion */
    frame = av_frame_alloc();
    rgb_frame = av_frame_alloc();
    if (!frame || !rgb_frame) { fprintf(stderr, "Failed to alloc frames\n"); goto err; }

    int width = 0, height = 0;
    uint8_t *picture = NULL;

    /* Simple NAL unit parser based on start codes */
    long pos = 0;
    while (pos < len) {
        /* find next start code */
        long start = -1, end = -1;
        for (long i = pos; i + 3 < len; ++i) {
            if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) { start = i + 3; break; }
            if (i + 4 < len && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) { start = i + 4; break; }
        }
        if (start == -1) break; /* no more NALs */
        /* find next start code after start */
        for (long i = start; i + 3 < len; ++i) {
            if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) { end = i; break; }
            if (i + 4 < len && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) { end = i; break; }
        }
        if (end == -1) end = len;

        uint8_t *nal_ptr = buf + start;
        uint32_t nal_len = (uint32_t)(end - start);

        /* feed this NAL to decoder */
        uint32_t status = h264bsdDecode(storage, nal_ptr, nal_len, &picture, (u32*)&width, (u32*)&height);
        if (status == H264BSD_PIC_RDY && picture && width > 0 && height > 0) {
            /* Set up frame data pointers assuming YUV420 planar layout */
            frame->data[0] = picture;
            frame->data[1] = picture + width * height;
            frame->data[2] = frame->data[1] + (width * height) / 4;
            frame->linesize[0] = width;
            frame->linesize[1] = width / 2;
            frame->linesize[2] = width / 2;

            /* Prepare RGB buffer */
            int rgb_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
            uint8_t *local_rgb_buf = av_malloc(rgb_buffer_size);
            if (!local_rgb_buf) { fprintf(stderr, "Failed to alloc rgb buffer\n"); goto err; }
            if (av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, local_rgb_buf, AV_PIX_FMT_RGB24, width, height, 1) < 0) {
                av_free(local_rgb_buf);
                fprintf(stderr, "Failed to fill rgb arrays\n"); goto err; }

            /* Create/refresh sws context for this resolution */
            if (sws_ctx) sws_freeContext(sws_ctx);
            sws_ctx = sws_getContext(width, height, AV_PIX_FMT_YUV420P, width, height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, NULL, NULL, NULL);
            if (!sws_ctx) { av_free(local_rgb_buf); fprintf(stderr, "Failed to create sws context\n"); goto err; }

            sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0, height, rgb_frame->data, rgb_frame->linesize);

            /* Initialize video output if not already */
            if (!video) {
                if (init_video_output(width, height) < 0) {
                    av_free(local_rgb_buf);
                    goto err;
                }
            }

            video_draw(video, rgb_frame->data[0], rgb_frame->linesize[0]);
            frames_displayed++;

            av_free(local_rgb_buf);
        }

        pos = end;
    }

    /* cleanup */
    h264bsdShutdown(storage);
    h264bsdFree(storage);
    free(buf);
    return 0;

err:
    if (storage) { h264bsdShutdown(storage); h264bsdFree(storage); }
    free(buf);
    return -1;
}
#endif

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
    int video_stream_idx = -1;
    AVPacket packet; /* stack packet - avoid holding packet memory between iterations */
    int should_quit = 0;
    
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
    
    // Initialize FFmpeg and open stream
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
            /* Pre-decode frameskip: drop incoming packets before decoding
             * when skip_remaining indicates we should skip frames. This
             * avoids the cost of decoding frames we're going to discard.
             *
             * IMPORTANT: do NOT drop keyframes — they are needed as references
             * for subsequent frames. Only drop non-key packets.
             */
#ifndef DISABLE_FRAMESKIP
            if (FRAMESKIP_AMOUNT > 0 && skip_remaining > 0) {
                /* Keep packets that are keyframes to preserve decoder references */
                if (!(packet.flags & AV_PKT_FLAG_KEY)) {
                    frames_dropped++;
                    skip_remaining--;
                    av_packet_unref(&packet);
                    continue; /* skip decoding this non-key packet */
                }
                /* else: allow keyframe through so decoder state remains valid */
            }
#endif
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
                    frames_dropped++;
                    skip_remaining--;
                    // Do not update last_frame_time here; keep timing anchored to last shown frame
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
                          frame->linesize, 0, codec_ctx->height,
                          rgb_frame->data, rgb_frame->linesize);
#endif
                
                // Draw frame
                video_draw(video, rgb_frame->data[0], rgb_frame->linesize[0]);
                frames_displayed++;
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
    
    printf("Playback finished\n");
    if (frames_displayed > 0) {
        printf("Performance: %d frames displayed, %d frames dropped (%.1f%% drop rate)\n", 
               frames_displayed, frames_dropped, 
               (float)frames_dropped / (frames_displayed + frames_dropped) * 100.0f);
    }
    
    // Cleanup
    free(stream_url);
    cleanup_resources();

    return 0;
}
