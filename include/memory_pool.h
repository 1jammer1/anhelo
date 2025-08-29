#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stddef.h>

#define POOL_BLOCK_SIZE 8192  // 8KB blocks
#define FRAME_POOL_SIZE 4     // Pool of 4 frames

// Memory pool for small allocations
typedef struct memory_pool {
    char *pool;
    size_t pool_size;
    size_t used;
    struct memory_pool *next;
} memory_pool_t;

// Frame buffer pool
typedef struct {
    void *frames[FRAME_POOL_SIZE];
    void *rgb_buffers[FRAME_POOL_SIZE];
    int available[FRAME_POOL_SIZE];
    int pool_size;
} frame_pool_t;

// Memory pool functions
memory_pool_t *pool_create(size_t initial_size);
void *pool_alloc(memory_pool_t *pool, size_t size);
void pool_reset(memory_pool_t *pool);
void pool_destroy(memory_pool_t *pool);

// Frame pool functions
frame_pool_t *frame_pool_create(int width, int height, int pool_size);
void *get_frame_from_pool(frame_pool_t *pool);
void *get_rgb_buffer_from_pool(frame_pool_t *pool, int index);
void return_frame_to_pool(frame_pool_t *pool, void *frame);
void frame_pool_destroy(frame_pool_t *pool);

#endif // MEMORY_POOL_H
