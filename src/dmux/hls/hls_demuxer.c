#include "../../../include/hls_demuxer.h"
#include <curl/curl.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h> // for usleep

// Internal buffer structure
struct hls_buffer {
    char *data;
    size_t size;
    size_t capacity;
};

// Internal callback for curl
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct hls_buffer *buf = (struct hls_buffer *)userp;

    if (buf->size + realsize > buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        if (new_capacity < buf->size + realsize) {
            new_capacity = buf->size + realsize + 1024;
        }
        char *new_data = realloc(buf->data, new_capacity);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    memcpy(buf->data + buf->size, contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';

    return realsize;
}

// Download URL to buffer (internal function)
static hls_error_t download_url(hls_demuxer_t *demuxer, const char *url, struct hls_buffer *buf) {
    CURL *curl;
    CURLcode res;
    
    curl = curl_easy_init();
    if (!curl) return HLS_ERROR_MEMORY;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, demuxer->user_agent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, demuxer->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        return HLS_ERROR_NETWORK;
    }
    
    return HLS_OK;
}

// Create demuxer
hls_demuxer_t* hls_demuxer_create(void) {
    hls_demuxer_t *demuxer = calloc(1, sizeof(hls_demuxer_t));
    if (!demuxer) return NULL;
    
    demuxer->user_agent = strdup("HLS-Demuxer/1.0");
    demuxer->timeout_ms = 10000;
    return demuxer;
}

// Destroy demuxer
void hls_demuxer_destroy(hls_demuxer_t *demuxer) {
    if (demuxer) {
        free(demuxer->user_agent);
        free(demuxer);
    }
}

// Create playlist
hls_playlist_t* hls_playlist_create(void) {
    hls_playlist_t *playlist = calloc(1, sizeof(hls_playlist_t));
    if (!playlist) return NULL;
    
    playlist->segment_capacity = 16;
    playlist->segments = calloc(playlist->segment_capacity, sizeof(hls_segment_t));
    if (!playlist->segments) {
        free(playlist);
        return NULL;
    }
    
    return playlist;
}

// Destroy playlist
void hls_playlist_destroy(hls_playlist_t *playlist) {
    if (!playlist) return;
    
    for (size_t i = 0; i < playlist->segment_count; i++) {
        free(playlist->segments[i].url);
        free(playlist->segments[i].key_url);
        free(playlist->segments[i].key_iv);
    }
    free(playlist->segments);
    free(playlist->base_url);
    if (playlist->variants) {
        for (size_t i = 0; i < playlist->variant_count; i++) {
            free(playlist->variants[i]);
        }
        free(playlist->variants);
    }
    free(playlist);
}

// Get base URL from full URL
char* hls_resolve_url(const char *base_url, const char *relative_url) {
    if (!base_url || !relative_url) return NULL;
    
    // If relative_url is already absolute
    if (strncmp(relative_url, "http://", 7) == 0 || strncmp(relative_url, "https://", 8) == 0) {
        return strdup(relative_url);
    }
    
    // Simple path resolution (basic implementation)
    size_t base_len = strlen(base_url);
    size_t rel_len = strlen(relative_url);
    char *result = malloc(base_len + rel_len + 2);
    if (!result) return NULL;
    
    strcpy(result, base_url);
    char *last_slash = strrchr(result, '/');
    if (last_slash) {
        last_slash[1] = '\0';
    }
    strcat(result, relative_url);
    
    return result;
}

// Process entire stream
// Process HLS stream continuously until callback signals quit or error
hls_error_t hls_process_stream(hls_demuxer_t *demuxer, const char *playlist_url, hls_segment_callback_t callback, void *user_data) {
    if (!demuxer || !playlist_url || !callback) {
        return HLS_ERROR_PARSE;
    }
    demuxer->segment_callback = callback;
    demuxer->callback_user_data = user_data;

    size_t last_count = 0;
    char *last_processed_url = NULL; // track the URL of the last segment we processed
    while (1) {
        // Download playlist
        struct hls_buffer buf = {0};
        buf.capacity = 4096;
        buf.data = malloc(buf.capacity);
        if (!buf.data) return HLS_ERROR_MEMORY;
        hls_error_t err = download_url(demuxer, playlist_url, &buf);
        if (err != HLS_OK) {
            free(buf.data);
            return err;
        }

        // Parse playlist
        hls_playlist_t *playlist = hls_playlist_create();
        if (!playlist) {
            free(buf.data);
            return HLS_ERROR_MEMORY;
        }
    char *base_url = NULL;
    char *last_slash = strrchr(playlist_url, '/');
    if (last_slash) base_url = strndup(playlist_url, last_slash - playlist_url + 1);
    // Parser will duplicate base_url into playlist->base_url, do not assign directly to avoid double-free
    err = hls_parse_playlist_from_memory(demuxer, buf.data, buf.size, base_url, playlist);
        if (err != HLS_OK) {
            hls_playlist_destroy(playlist);
            free(buf.data);
            free(base_url);
            return err;
        }

        // Determine starting index based on last processed URL (handles sliding windows)
        size_t start_index = 0;
        if (last_processed_url) {
            for (size_t i = 0; i < playlist->segment_count; i++) {
                if (playlist->segments[i].url && strcmp(playlist->segments[i].url, last_processed_url) == 0) {
                    start_index = i + 1; // start after the last processed one
                    break;
                }
            }
        } else {
            start_index = last_count; // initial fallback
        }
        if (start_index > playlist->segment_count) start_index = playlist->segment_count;

        // Process new segments
        for (size_t i = start_index; i < playlist->segment_count; i++) {
            hls_segment_t *segment = &playlist->segments[i];
            char *segment_url = hls_resolve_url(playlist->base_url, segment->url);
            if (!segment_url) continue;
            struct hls_buffer seg_buf = {0};
            seg_buf.capacity = 1024*1024;
            seg_buf.data = malloc(seg_buf.capacity);
            if (!seg_buf.data) {
                free(segment_url);
                continue;
            }
            hls_error_t seg_err = download_url(demuxer, segment_url, &seg_buf);
            if (seg_err == HLS_OK && seg_buf.size > 0) {
                int cb = callback((const unsigned char*)seg_buf.data, seg_buf.size, user_data);
                if (cb) {
                    // Cleanup and exit on callback request
                    free(seg_buf.data);
                    free(segment_url);
                    if (last_processed_url) { free(last_processed_url); last_processed_url = NULL; }
                    hls_playlist_destroy(playlist);
                    free(buf.data);
                    free(base_url);
                    return HLS_OK;
                }
            }
            free(seg_buf.data);
            free(segment_url);
            // Update last processed URL to current segment
            if (last_processed_url) { free(last_processed_url); last_processed_url = NULL; }
            if (segment->url) last_processed_url = strdup(segment->url);
        }
        last_count = playlist->segment_count;
        hls_playlist_destroy(playlist);
        free(buf.data);
        free(base_url);

        // Wait before refreshing playlist
        usleep(500000); // 0.5s
    }
}

// Error string conversion
const char* hls_get_error_string(hls_error_t error) {
    switch (error) {
        case HLS_OK: return "Success";
        case HLS_ERROR_MEMORY: return "Memory allocation failed";
        case HLS_ERROR_NETWORK: return "Network error";
        case HLS_ERROR_PARSE: return "Parse error";
        case HLS_ERROR_IO: return "I/O error";
        default: return "Unknown error";
    }
}

// Check if playlist is master
bool hls_is_master_playlist(const char *data, size_t len) {
    if (!data) return false;
    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        // trim leading spaces
        const char *s = p;
        while (s < line_end && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        if ((size_t)(line_end - s) >= 19 && strncmp(s, "#EXT-X-STREAM-INF:", 19) == 0) {
            return true;
        }
        p = nl ? nl + 1 : end;
    }
    return false;
}