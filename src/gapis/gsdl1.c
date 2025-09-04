#include "../include/video.h"
#include <SDL/SDL.h>
#include <stdlib.h>
#include <string.h>

struct video_t {
    SDL_Surface *screen;
    int video_width;
    int video_height;
    int window_width;
    int window_height;
    int video_x, video_y;
    int copy_width, copy_height;
    int bytes_per_pixel;
    int screen_pitch;
    uint8_t *screen_pixels;
#ifdef MINIMAL_MEMORY_BUFFERS
    // Reuse memory for row operations to reduce allocations
    uint8_t *row_buffer;
#endif
};

video_t *video_create(int width, int height) {
    // Use calloc instead of malloc to avoid initialization overhead
    video_t *v = calloc(1, sizeof(video_t));
    if (!v) return NULL;
    
    // Get the existing screen surface created by main.c
    v->screen = SDL_GetVideoSurface();
    if (!v->screen) {
        free(v);
        return NULL;
    }
    
    v->video_width = width;
    v->video_height = height;
    v->window_width = v->screen->w;
    v->window_height = v->screen->h;
    
    // Pre-calculate video position (centered horizontally, bottom vertically)
    v->video_x = (v->window_width - v->video_width) / 2;
    v->video_y = v->window_height - v->video_height;
    
    // Ensure video doesn't go outside window bounds
    if (v->video_x < 0) v->video_x = 0;
    if (v->video_y < 0) v->video_y = 0;
    if (v->video_x + v->video_width > v->window_width) {
        v->video_x = v->window_width - v->video_width;
    }
    
    // Pre-calculate copy dimensions
    v->copy_width = v->video_width;
    v->copy_height = v->video_height;
    if (v->video_x + v->copy_width > v->window_width) {
        v->copy_width = v->window_width - v->video_x;
    }
    if (v->video_y + v->copy_height > v->window_height) {
        v->copy_height = v->window_height - v->video_y;
    }
    
    // Cache screen surface properties for performance
    v->screen_pixels = (uint8_t *)v->screen->pixels;
    v->screen_pitch = v->screen->pitch;
    v->bytes_per_pixel = v->screen->format->BytesPerPixel;
    
    printf("SDL optimized: %dx%d window, video: %dx%d at (%d,%d), copy: %dx%d\n", 
           v->window_width, v->window_height, v->video_width, v->video_height, 
           v->video_x, v->video_y, v->copy_width, v->copy_height);
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Allocate a single row buffer for pixel operations
    v->row_buffer = malloc(v->video_width * 3);
    if (!v->row_buffer) {
        printf("Warning: Could not allocate row buffer, falling back to direct mode\n");
    }
#endif
    
    return v;
}

void video_draw(video_t *v, const uint8_t *rgb, int linesize) {
    if (!v || !v->screen || !rgb || linesize <= 0) return;
    
    // Validate that linesize is reasonable for RGB24 format
    int min_linesize = v->video_width * 3;
    if (linesize < min_linesize) return;
    
    // Lock surface for pixel manipulation
    if (SDL_MUSTLOCK(v->screen)) {
        if (SDL_LockSurface(v->screen) < 0) return;
    }
    
    // Clear screen to black (single memset for performance)
#ifdef MINIMAL_MEMORY_BUFFERS
    // In low memory mode, clear only the visible area
    SDL_Rect clear_rect = {0, 0, v->window_width, v->window_height};
    SDL_FillRect(v->screen, &clear_rect, 0);
#else
    memset(v->screen_pixels, 0, v->screen_pitch * v->window_height);
#endif
    
    // Fast pixel copying using cached values
    const int bytes_per_pixel = v->bytes_per_pixel;
    const int screen_pitch = v->screen_pitch;
    
    if (bytes_per_pixel == 3) {
        // Optimized 24-bit RGB format copying
#ifdef MINIMAL_MEMORY_BUFFERS
        // Process one row at a time to reduce memory pressure
        for (int y = 0; y < v->copy_height; y++) {
            const uint8_t *src_row = rgb + (y * linesize);
            uint8_t *dst_row = v->screen_pixels + ((v->video_y + y) * screen_pitch) + (v->video_x * 3);
            
            // Process 1 pixel at a time (slower but less memory)
            for (int x = 0; x < v->copy_width; x++) {
                dst_row[x*3+0] = src_row[x*3+2];    // B
                dst_row[x*3+1] = src_row[x*3+1];    // G  
                dst_row[x*3+2] = src_row[x*3+0];    // R
            }
        }
#else
        // Optimized 24-bit RGB format copying
        for (int y = 0; y < v->copy_height; y++) {
            const uint8_t *src_row = rgb + (y * linesize);
            uint8_t *dst_row = v->screen_pixels + ((v->video_y + y) * screen_pitch) + (v->video_x * 3);
            
            // Unrolled inner loop for better performance
            int x = 0;
            for (; x < v->copy_width - 3; x += 4) {
                // Process 4 pixels at once
                dst_row[x*3+0] = src_row[x*3+2];    dst_row[x*3+1] = src_row[x*3+1];    dst_row[x*3+2] = src_row[x*3+0];    // BGR
                dst_row[x*3+3] = src_row[x*3+5];    dst_row[x*3+4] = src_row[x*3+4];    dst_row[x*3+5] = src_row[x*3+3];
                dst_row[x*3+6] = src_row[x*3+8];    dst_row[x*3+7] = src_row[x*3+7];    dst_row[x*3+8] = src_row[x*3+6];
                dst_row[x*3+9] = src_row[x*3+11];   dst_row[x*3+10] = src_row[x*3+10];  dst_row[x*3+11] = src_row[x*3+9];
            }
            // Handle remaining pixels
            for (; x < v->copy_width; x++) {
                dst_row[x*3+0] = src_row[x*3+2];    // B
                dst_row[x*3+1] = src_row[x*3+1];    // G  
                dst_row[x*3+2] = src_row[x*3+0];    // R
            }
        }
#endif
    } else if (bytes_per_pixel == 4) {
        // Optimized 32-bit RGBA format copying
        for (int y = 0; y < v->copy_height; y++) {
            const uint8_t *src_row = rgb + (y * linesize);
            uint8_t *dst_row = v->screen_pixels + ((v->video_y + y) * screen_pitch) + (v->video_x * 4);
            
            for (int x = 0; x < v->copy_width; x++) {
                dst_row[x*4+0] = src_row[x*3+2];    // B
                dst_row[x*4+1] = src_row[x*3+1];    // G
                dst_row[x*4+2] = src_row[x*3+0];    // R
                dst_row[x*4+3] = 255;               // A
            }
        }
    } else {
        // Unsupported format - draw gray rectangle as fallback
        SDL_Rect fallback_rect = {v->video_x, v->video_y, v->copy_width, v->copy_height};
        SDL_FillRect(v->screen, &fallback_rect, SDL_MapRGB(v->screen->format, 64, 64, 64));
    }
    
    // Unlock surface
    if (SDL_MUSTLOCK(v->screen)) {
        SDL_UnlockSurface(v->screen);
    }
    
    // Update the display
    SDL_Flip(v->screen);
}

int video_poll(video_t *v) {
    if (!v) return 1;
    
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return 1; // Signal to quit
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    return 1; // Signal to quit
                }
                break;
        }
    }
    
    return 0; // Continue running
}

void video_destroy(video_t *v) {
    if (!v) return;
    
#ifdef MINIMAL_MEMORY_BUFFERS
    if (v->row_buffer) {
        free(v->row_buffer);
    }
#endif
    
    free(v);
}
