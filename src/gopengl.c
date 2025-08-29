#include "../include/video.h"
#include <SDL/SDL.h>
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>

// Helper function to find next power of 2
static int next_power_of_2(int n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;
    return n;
}

struct video_t {
    int video_width;
    int video_height;
    int window_width;
    int window_height;
#ifdef MINIMAL_MEMORY_BUFFERS
    // For low-memory environments, use smaller textures
    int texture_width;
    int texture_height;
#else
    int texture_width;   // Power-of-2 texture dimensions
    int texture_height;
#endif
    GLuint texture_id;
    GLuint display_list_id;
    int video_x, video_y;  // Pre-calculated position
    float tex_coords[8];   // Pre-calculated texture coordinates
    float vertices[8];     // Pre-calculated vertex positions
    int texture_initialized;
};

video_t *video_create(int width, int height) {
    video_t *v = calloc(1, sizeof(video_t));
    if (!v) return NULL;
    
    // Get the existing screen surface created by main.c
    SDL_Surface *screen = SDL_GetVideoSurface();
    if (!screen) {
        free(v);
        return NULL;
    }
    
    v->video_width = width;
    v->video_height = height;
    v->window_width = screen->w;
    v->window_height = screen->h;
    v->texture_initialized = 0;
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // For low-memory, use non-power-of-2 textures if possible, or minimum POT
    v->texture_width = width;
    v->texture_height = height;
#else
    // Use power-of-2 textures for better GPU memory efficiency
    v->texture_width = next_power_of_2(width);
    v->texture_height = next_power_of_2(height);
#endif
    
    // Pre-calculate video position (centered horizontally, bottom vertically)
    v->video_x = (v->window_width - v->video_width) / 2;
    v->video_y = v->window_height - v->video_height;
    
    // Ensure video doesn't go outside window bounds
    if (v->video_x < 0) v->video_x = 0;
    if (v->video_y < 0) v->video_y = 0;
    if (v->video_x + v->video_width > v->window_width) {
        v->video_x = v->window_width - v->video_width;
    }
    
    // Pre-calculate texture coordinates (map actual video size to power-of-2 texture)
    float tex_u = (float)v->video_width / v->texture_width;
    float tex_v = (float)v->video_height / v->texture_height;
    v->tex_coords[0] = 0.0f;  v->tex_coords[1] = 0.0f;   // Bottom-left
    v->tex_coords[2] = tex_u; v->tex_coords[3] = 0.0f;   // Bottom-right
    v->tex_coords[4] = tex_u; v->tex_coords[5] = tex_v;  // Top-right
    v->tex_coords[6] = 0.0f;  v->tex_coords[7] = tex_v;  // Top-left
    
    // Pre-calculate vertex positions
    v->vertices[0] = (float)v->video_x;                        v->vertices[1] = (float)v->video_y;                         // Bottom-left
    v->vertices[2] = (float)(v->video_x + v->video_width);     v->vertices[3] = (float)v->video_y;                         // Bottom-right
    v->vertices[4] = (float)(v->video_x + v->video_width);     v->vertices[5] = (float)(v->video_y + v->video_height);     // Top-right
    v->vertices[6] = (float)v->video_x;                        v->vertices[7] = (float)(v->video_y + v->video_height);     // Top-left
    
    // Initialize OpenGL state once
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDisable(GL_FOG);
    glDisable(GL_STENCIL_TEST);
    
    // Set clear color to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    
    // Generate and configure texture for maximum performance
    glGenTextures(1, &v->texture_id);
    glBindTexture(GL_TEXTURE_2D, v->texture_id);
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Disable features that consume memory
    glDisable(GL_TEXTURE_2D);  // Only enable when needed
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
    glHint(GL_POINT_SMOOTH_HINT, GL_FASTEST);
    glHint(GL_LINE_SMOOTH_HINT, GL_FASTEST);
    glHint(GL_POLYGON_SMOOTH_HINT, GL_FASTEST);
    glHint(GL_FOG_HINT, GL_FASTEST);
    // Use nearest filtering for textures
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#else
    // Use nearest filtering for better performance than linear
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
#endif
    
    // Set up viewport and projection matrix
    glViewport(0, 0, v->window_width, v->window_height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, v->window_width, v->window_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    // Create display list for quad rendering
    v->display_list_id = glGenLists(1);
    glNewList(v->display_list_id, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        glTexCoord2f(v->tex_coords[i*2], v->tex_coords[i*2+1]);
        glVertex2f(v->vertices[i*2], v->vertices[i*2+1]);
    }
    glEnd();
    glEndList();
    
    // Set optimized pixel store parameters
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    
    // Test OpenGL by clearing and swapping buffers
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapBuffers();
    
    printf("OpenGL optimized: %dx%d window, video: %dx%d at (%d,%d)\n", 
           v->window_width, v->window_height, v->video_width, v->video_height, v->video_x, v->video_y);
    
    return v;
}

void video_draw(video_t *v, const uint8_t *rgb, int linesize) {
    if (!v || !rgb || linesize <= 0) return;
    
    // Validate that linesize is reasonable for RGB24 format
    int min_linesize = v->video_width * 3;
    if (linesize < min_linesize) return;
    
    // Clear screen (minimal state changes)
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Bind texture only once per frame
    glBindTexture(GL_TEXTURE_2D, v->texture_id);
    
    // Set pixel store parameters only if needed
    if (linesize != v->video_width * 3) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / 3);
    }
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Enable texture only when needed
    glEnable(GL_TEXTURE_2D);
    
    // In low-memory mode, use smallest possible texture
    if (v->texture_initialized) {
        // Update existing texture
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->video_width, v->video_height,
                        GL_RGB, GL_UNSIGNED_BYTE, rgb);
    } else {
        // First frame: allocate minimum texture
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, v->texture_width, v->texture_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->video_width, v->video_height,
                        GL_RGB, GL_UNSIGNED_BYTE, rgb);
        v->texture_initialized = 1;
    }
    
    // Draw and disable texture to save state
    glCallList(v->display_list_id);
    glDisable(GL_TEXTURE_2D);
#else
    // Use glTexSubImage2D for better performance after first frame
    if (v->texture_initialized) {
        // Upload only the video region into the POT texture
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->video_width, v->video_height,
                        GL_RGB, GL_UNSIGNED_BYTE, rgb);
    } else {
        // First frame: allocate full power-of-two texture but upload video data into it
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, v->texture_width, v->texture_height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        // Upload the video into the lower-left corner of the POT texture
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v->video_width, v->video_height,
                        GL_RGB, GL_UNSIGNED_BYTE, rgb);
        v->texture_initialized = 1;
    }
#endif
    
    // Reset row length if it was changed
    if (linesize != v->video_width * 3) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    
    // Set color once (avoid redundant state changes)
    glColor3f(1.0f, 1.0f, 1.0f);
    
    // Use pre-compiled display list for maximum performance
    glCallList(v->display_list_id);
    
    // Swap buffers to display the frame
    SDL_GL_SwapBuffers();
}

int video_poll(video_t *v) {
    (void)v; // Suppress unused parameter warning
    
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
    
    if (v->texture_id) {
        glDeleteTextures(1, &v->texture_id);
    }
    
    if (v->display_list_id) {
        glDeleteLists(v->display_list_id, 1);
    }
    
    free(v);
}
