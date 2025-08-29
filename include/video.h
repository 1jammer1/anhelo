// Minimal video backend API used by main.c
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

typedef struct video_t video_t;

// create video output of width x height. Returns NULL on error.
video_t *video_create(int width, int height);

// draw a RGB24 buffer (linesize bytes per row)
void video_draw(video_t *v, const uint8_t *rgb, int linesize);

// poll events, returns 0 to continue, non-zero to quit
int video_poll(video_t *v);

// destroy
void video_destroy(video_t *v);

#endif
