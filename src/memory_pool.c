#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef NO_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#endif

#include "../include/memory_pool.h"

// Memory pool implementation
memory_pool_t *pool_create(size_t initial_size) {
    memory_pool_t *pool = malloc(sizeof(memory_pool_t));
    if (!pool) return NULL;
    
    pool->pool_size = initial_size > POOL_BLOCK_SIZE ? initial_size : POOL_BLOCK_SIZE;
    pool->pool = malloc(pool->pool_size);
    if (!pool->pool) {
        free(pool);
        return NULL;
    }
    
    pool->used = 0;
    pool->next = NULL;
    return pool;
}

void *pool_alloc(memory_pool_t *pool, size_t size) {
    if (!pool) return malloc(size); // Fallback to regular malloc
    
    // Align to 8-byte boundaries
    size = (size + 7) & ~7;
    
    // Check if current pool has enough space
    if (pool->used + size <= pool->pool_size) {
        void *ptr = pool->pool + pool->used;
        pool->used += size;
        return ptr;
    }
    
    // Need a new pool block
    if (!pool->next) {
        size_t new_size = size > POOL_BLOCK_SIZE ? size : POOL_BLOCK_SIZE;
        pool->next = pool_create(new_size);
        if (!pool->next) return malloc(size); // Fallback
    }
    
    return pool_alloc(pool->next, size);
}

void pool_reset(memory_pool_t *pool) {
    while (pool) {
        pool->used = 0;
        pool = pool->next;
    }
}

void pool_destroy(memory_pool_t *pool) {
    while (pool) {
        memory_pool_t *next = pool->next;
        free(pool->pool);
        free(pool);
        pool = next;
    }
}

// Frame pool implementation
frame_pool_t *frame_pool_create(int width, int height, int pool_size) {
    if (pool_size > FRAME_POOL_SIZE) pool_size = FRAME_POOL_SIZE;
    
    frame_pool_t *pool = malloc(sizeof(frame_pool_t));
    if (!pool) return NULL;
    
    pool->pool_size = pool_size;
    memset(pool->available, 1, sizeof(pool->available)); // All available initially
    
    // Pre-allocate frames and RGB buffers
    for (int i = 0; i < pool_size; i++) {
#ifndef NO_FFMPEG
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            // Cleanup on failure
            for (int j = 0; j < i; j++) {
                av_frame_free((AVFrame**)&pool->frames[j]);
                av_free(pool->rgb_buffers[j]);
            }
            free(pool);
            return NULL;
        }
        
        // Allocate RGB buffer
        int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        uint8_t *rgb_buffer = av_malloc(rgb_size);
        if (!rgb_buffer) {
            av_frame_free(&frame);
            // Cleanup on failure
            for (int j = 0; j < i; j++) {
                av_frame_free((AVFrame**)&pool->frames[j]);
                av_free(pool->rgb_buffers[j]);
            }
            free(pool);
            return NULL;
        }
        
        pool->frames[i] = frame;
        pool->rgb_buffers[i] = rgb_buffer;
#ifndef NO_FFMPEG
        pool->rgb_sizes[i] = rgb_size;
#endif
#else
        // For NO_FFMPEG, just allocate basic buffers
        int rgb_size = width * height * 3; // RGB24
        uint8_t *rgb_buffer = malloc(rgb_size);
        if (!rgb_buffer) {
            // Cleanup on failure
            for (int j = 0; j < i; j++) {
                free(pool->rgb_buffers[j]);
            }
            free(pool);
            return NULL;
        }
        
        pool->frames[i] = NULL; // No FFmpeg frames
        pool->rgb_buffers[i] = rgb_buffer;
#endif
    }
    
    return pool;
}

void *get_frame_from_pool(frame_pool_t *pool) {
    if (!pool) return NULL;
    
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->available[i]) {
            pool->available[i] = 0;
            return pool->frames[i];
        }
    }
    
    // Pool exhausted, return NULL (caller should handle)
    return NULL;
}

void *get_rgb_buffer_from_pool(frame_pool_t *pool, int index) {
    if (!pool || index < 0 || index >= pool->pool_size) return NULL;
    return pool->rgb_buffers[index];
}

void return_frame_to_pool(frame_pool_t *pool, void *frame) {
    if (!pool || !frame) return;
    
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->frames[i] == frame) {
            pool->available[i] = 1;
            return;
        }
    }
}

void frame_pool_destroy(frame_pool_t *pool) {
    if (!pool) return;
    
    for (int i = 0; i < pool->pool_size; i++) {
#ifndef NO_FFMPEG
        if (pool->frames[i]) {
            av_frame_free((AVFrame**)&pool->frames[i]);
        }
        if (pool->rgb_buffers[i]) {
            av_free(pool->rgb_buffers[i]);
        }
#else
        if (pool->rgb_buffers[i]) {
            free(pool->rgb_buffers[i]);
        }
#endif
    }
    
    free(pool);
}
