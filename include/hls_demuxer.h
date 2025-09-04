#ifndef HLS_DEMUXER_H
#define HLS_DEMUXER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Playlist types
typedef enum {
    HLS_PLAYLIST_UNKNOWN = 0,
    HLS_PLAYLIST_MASTER,
    HLS_PLAYLIST_MEDIA
} hls_playlist_type_t;

// Segment information
typedef struct {
    char *url;
    double duration;
    bool is_key_segment;
    char *key_url;
    char *key_iv;
} hls_segment_t;

// Playlist structure
typedef struct {
    hls_playlist_type_t type;
    char *base_url;
    hls_segment_t *segments;
    size_t segment_count;
    size_t segment_capacity;
    char **variants;  // For master playlists
    size_t variant_count;
} hls_playlist_t;

// Callback for segment data
typedef int (*hls_segment_callback_t)(const unsigned char *data, size_t size, void *user_data);

// Main demuxer context
typedef struct {
    char *user_agent;
    long timeout_ms;
    hls_segment_callback_t segment_callback;
    void *callback_user_data;
} hls_demuxer_t;

// Error codes
typedef enum {
    HLS_OK = 0,
    HLS_ERROR_MEMORY,
    HLS_ERROR_NETWORK,
    HLS_ERROR_PARSE,
    HLS_ERROR_IO
} hls_error_t;

// Create/destroy demuxer
hls_demuxer_t* hls_demuxer_create(void);
void hls_demuxer_destroy(hls_demuxer_t *demuxer);

// Playlist management
hls_playlist_t* hls_playlist_create(void);
void hls_playlist_destroy(hls_playlist_t *playlist);

// Parsing functions
hls_error_t hls_parse_playlist(hls_demuxer_t *demuxer, const char *url, hls_playlist_t *playlist);
hls_error_t hls_parse_playlist_from_memory(hls_demuxer_t *demuxer, const char *data, size_t len, const char *base_url, hls_playlist_t *playlist);

// Stream processing
hls_error_t hls_process_stream(hls_demuxer_t *demuxer, const char *playlist_url, hls_segment_callback_t callback, void *user_data);

// Utility functions
const char* hls_get_error_string(hls_error_t error);
bool hls_is_master_playlist(const char *data, size_t len);
char* hls_resolve_url(const char *base_url, const char *relative_url);

#ifdef __cplusplus
}
#endif

#endif // HLS_DEMUXER_H