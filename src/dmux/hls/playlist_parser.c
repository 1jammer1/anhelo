#include "../../../include/hls_demuxer.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <curl/curl.h>

// Internal buffer structure (also defined in hls_demuxer.c for internal use)
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

// Trim whitespace from string
static char* trim(char *str) {
    char *end;
    
    // Trim leading space
    while(isspace((unsigned char)*str)) str++;
    
    if(*str == 0) return str;
    
    // Trim trailing space
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    
    // Write new null terminator
    *(end+1) = 0;
    
    return str;
}

// Parse playlist from memory
hls_error_t hls_parse_playlist_from_memory(hls_demuxer_t *demuxer, const char *data, size_t len, const char *base_url, hls_playlist_t *playlist) {
    if (!data || !playlist) return HLS_ERROR_PARSE;
    
    if (base_url) {
        playlist->base_url = strdup(base_url);
    }
    
    const char *line = data;
    double current_duration = 0.0;
    
    while (line < data + len) {
        const char *next_line = strchr(line, '\n');
        if (!next_line) next_line = data + len;
        
        size_t line_len = next_line - line;
        char *line_copy = strndup(line, line_len);
        if (!line_copy) return HLS_ERROR_MEMORY;
        
        char *trimmed = trim(line_copy);
        
        if (strncmp(trimmed, "#EXTM3U", 7) == 0) {
            // Valid HLS playlist
        } else if (strncmp(trimmed, "#EXT-X-STREAM-INF:", 18) == 0) {
            playlist->type = HLS_PLAYLIST_MASTER;
            // Next line should be variant URL
        } else if (strncmp(trimmed, "#EXTINF:", 8) == 0) {
            playlist->type = HLS_PLAYLIST_MEDIA;
            // Parse duration
            current_duration = atof(trimmed + 8);
        } else if (strncmp(trimmed, "#EXT-X-TARGETDURATION:", 22) == 0) {
            playlist->type = HLS_PLAYLIST_MEDIA;
        } else if (strncmp(trimmed, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
            playlist->type = HLS_PLAYLIST_MEDIA;
        } else if (trimmed[0] != '#' && strlen(trimmed) > 0) {
            // This is a segment URL
            if (playlist->type == HLS_PLAYLIST_MASTER) {
                // Add to variants
                playlist->variants = realloc(playlist->variants, (playlist->variant_count + 1) * sizeof(char*));
                if (playlist->variants) {
                    playlist->variants[playlist->variant_count] = strdup(trimmed);
                    if (playlist->variants[playlist->variant_count]) {
                        playlist->variant_count++;
                    }
                }
            } else {
                // Add to segments
                if (playlist->segment_count >= playlist->segment_capacity) {
                    playlist->segment_capacity *= 2;
                    hls_segment_t *temp = realloc(playlist->segments, playlist->segment_capacity * sizeof(hls_segment_t));
                    if (!temp) {
                        free(line_copy);
                        return HLS_ERROR_MEMORY;
                    }
                    playlist->segments = temp;
                }
                
                hls_segment_t *seg = &playlist->segments[playlist->segment_count];
                memset(seg, 0, sizeof(hls_segment_t));
                seg->url = strdup(trimmed);
                seg->duration = current_duration;
                if (seg->url) {
                    playlist->segment_count++;
                }
            }
        }
        
        free(line_copy);
        line = next_line + 1;
    }
    
    return HLS_OK;
}

// Parse playlist from URL
hls_error_t hls_parse_playlist(hls_demuxer_t *demuxer, const char *url, hls_playlist_t *playlist) {
    struct hls_buffer buf = {0};
    buf.capacity = 4096;
    buf.data = malloc(buf.capacity);
    if (!buf.data) return HLS_ERROR_MEMORY;
    
    hls_error_t err = download_url(demuxer, url, &buf);
    if (err != HLS_OK) {
        free(buf.data);
        return err;
    }
    
    char *base_url = NULL;
    char *last_slash = strrchr(url, '/');
    if (last_slash) {
        base_url = strndup(url, last_slash - url + 1);
    }
    
    err = hls_parse_playlist_from_memory(demuxer, buf.data, buf.size, base_url, playlist);
    
    free(buf.data);
    free(base_url);
    return err;
}