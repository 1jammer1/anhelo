#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <curl/curl.h>

#include "../../include/twitch.h"

#include <unistd.h>

struct mem { 
    char *data; 
    size_t size; 
    size_t capacity; 
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct mem *m = (struct mem *)userp;
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // More conservative growth in low-memory mode
    size_t needed = m->size + realsize + 1;
    if (needed > m->capacity) {
        // Linear growth to avoid large allocations
        size_t new_capacity = needed + 1024;
        
        char *tmp = realloc(m->data, new_capacity);
        if (!tmp) return 0;
        m->data = tmp;
        m->capacity = new_capacity;
    }
#else
    // Check if we need to grow the buffer
    size_t needed = m->size + realsize + 1;
    if (needed > m->capacity) {
        // Exponential growth to reduce realloc calls
        size_t new_capacity = m->capacity ? m->capacity : 1024;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        
        char *tmp = realloc(m->data, new_capacity);
        if (!tmp) return 0;
        m->data = tmp;
        m->capacity = new_capacity;
    }
#endif
    
    memcpy(m->data + m->size, ptr, realsize);
    m->size += realsize;
    m->data[m->size] = '\0';
    return realsize;
}

static char *fetch_url_content(const char *url, long timeout_seconds)
{
    if (!url) return NULL;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    struct mem m = {0}; // Initialize all fields to 0
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "anhelo-twitch/1.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_seconds);
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Add memory-saving curl options
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 16384); // Smaller receive buffer
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // Use compression if possible
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0); // Simpler protocol
    curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 1); // Limit connections
#endif
    
    CURLcode res = curl_easy_perform(c);
    long code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (res != CURLE_OK || code < 200 || code >= 300) {
        if (m.data) { free(m.data); }
        return NULL;
    }
    return m.data; 
}


static char *resolve_relative(const char *base, const char *ref)
{
    if (!ref) return NULL;
    if (strstr(ref, "http://") == ref || strstr(ref, "https://") == ref) return strdup(ref);
    if (!base) return strdup(ref);
    
    const char *slash = strrchr(base, '/');
    if (!slash) return strdup(ref);
    size_t prefix_len = slash - base + 1; 
    char *out = malloc(prefix_len + strlen(ref) + 1);
    if (!out) return NULL;
    memcpy(out, base, prefix_len);
    strcpy(out + prefix_len, ref);
    return out;
}




static char *pick_lowest_variant_from_master(const char *master_content, const char *master_url)
{
    if (!master_content) return NULL;
    const char *p = master_content;
    struct variant { char *uri; int bandwidth; int height; };
    struct variant *list = NULL; size_t n = 0;

    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t linelen = line_end ? (size_t)(line_end - p) : strlen(p);
        
        const char *ln = p;
        while (*ln == ' ' || *ln == '\t') { ln++; linelen--; }
        if (linelen >= 17 && strncmp(ln, "#EXT-X-STREAM-INF:", 17) == 0) {
            
            char *line = malloc(linelen + 1);
            if (!line) break;
            memcpy(line, ln, linelen);
            line[linelen] = '\0';
            int bw = 0, h = 0;
            
            char *bwpos = strstr(line, "BANDWIDTH=");
            if (bwpos) bw = atoi(bwpos + strlen("BANDWIDTH="));
            char *respos = strstr(line, "RESOLUTION=");
            if (respos) {
                respos += strlen("RESOLUTION=");
                /* width is not used, so skip parsing it */
                char *x = strchr(respos, 'x');
                if (x) h = atoi(x+1);
            }
            free(line);
            
            const char *q = line_end ? line_end + 1 : NULL;
            while (q && *q) {
                
                const char *le = strchr(q, '\n');
                size_t l = le ? (size_t)(le - q) : strlen(q);
                
                const char *s = q; while (*s == ' ' || *s == '\t') s++, l--;
                if (l > 0 && *s != '#') {
                    
                    char *uri = malloc(l+1);
                    if (!uri) break;
                    memcpy(uri, s, l);
                    uri[l] = '\0';
                    
                    char *abs = resolve_relative(master_url, uri);
                    free(uri);
                    struct variant v = { abs, bw, h };
                    struct variant *tmp = realloc(list, (n+1)*sizeof(*list));
                    if (!tmp) { free(abs); break; }
                    list = tmp; list[n++] = v;
                    break;
                }
                q = le ? le + 1 : NULL;
            }
            p = line_end ? line_end + 1 : p + linelen;
            continue;
        }
        p = line_end ? line_end + 1 : NULL;
        if (!p) break;
    }

    if (n == 0) return NULL;
    
    int best_idx = 0;
    for (size_t i = 1; i < n; i++) {
        int h1 = list[i].height ? list[i].height : INT_MAX;
        int h0 = list[best_idx].height ? list[best_idx].height : INT_MAX;
        if (h1 < h0) best_idx = i;
        else if (h1 == h0) {
            
            if (list[i].bandwidth && list[best_idx].bandwidth && list[i].bandwidth < list[best_idx].bandwidth) best_idx = i;
        }
    }
    // Take ownership of the chosen URI instead of duplicating it.
    char *chosen = list[best_idx].uri;

    for (size_t i = 0; i < n; i++) {
        if (i != (size_t)best_idx) free(list[i].uri);
    }
    free(list);
    return chosen;
}


static char *extract_quoted_after(const char *hay, const char *anchor, const char *needle)
{
    if (!hay || !needle) return NULL;
    
    const char *start = hay;
    if (anchor) {
        start = strstr(hay, anchor);
        if (!start) return NULL;
    }
    const char *p = strstr(start, needle);
    if (!p) return NULL;
    p = strchr(p + strlen(needle), ':');
    if (!p) return NULL;
    p++;
    while (*p && (*p == ' ' || *p == '\n' || *p == '\t')) p++;
    if (*p == '"') p++; 
    const char *q = p;
    while (*q) {
        if (*q == '"') {
            
            const char *r = q - 1; int backslashes = 0;
            while (r >= p && *r == '\\') { backslashes++; r--; }
            if ((backslashes % 2) == 0) break; 
        }
        q++;
    }
    if (*q != '"') return NULL;
    size_t len = q - p;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    
    const char *s = p; char *d = out;
    size_t outidx = 0;
    while (s < q && outidx < len) {
        if (*s == '\\' && (s+1 < q) && (*(s+1) == '\\' || *(s+1) == '"')) { s++; }
        *d++ = *s++;
        outidx++;
    }
    *d = '\0';
    return out;
}



char *twitch_resolve(const char *input)
{
    if (!input) return NULL;
    const char *name = input;
    const char *p = strstr(input, "twitch.tv/");
    if (p) name = p + strlen("twitch.tv/");

    
    char channel[256] = {0};
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(channel); i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-') break;
        channel[i] = name[i];
    }
    channel[i] = '\0';
    if (i == 0) return NULL;

    
    const char *sha = "0828119ded1c13477966434e15800ff57ddacf13ba1911c129dc2200705b0712";
    char vars[512];
    snprintf(vars, sizeof(vars), "{\"isLive\":true,\"login\":\"%s\",\"isVod\":false,\"vodID\":\"\",\"playerType\":\"embed\"}", channel);
    vars[sizeof(vars)-1] = '\0';  

    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"operationName\":\"PlaybackAccessToken\",\"variables\":%s,\"extensions\":{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}}",
        vars, sha);
    payload[sizeof(payload)-1] = '\0';  
    
    
    struct mem m = {0}; // Initialize all fields including capacity
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    struct curl_slist *hdrs = NULL;
    const char *client_id = getenv("TWITCH_CLIENT_ID");
    if (!client_id) client_id = "kimne78kx3ncx6brgo4mv6wki5h1ko"; 
    char hdrbuf[512];
    snprintf(hdrbuf, sizeof(hdrbuf), "Client-ID: %s", client_id);
    hdrs = curl_slist_append(hdrs, hdrbuf);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    
#ifdef MINIMAL_MEMORY_BUFFERS
    // Add minimal headers for smaller requests
    hdrs = curl_slist_append(hdrs, "Accept-Encoding: gzip, deflate");
    hdrs = curl_slist_append(hdrs, "Connection: close"); // Don't keep connections
#endif
    
    curl_easy_setopt(c, CURLOPT_URL, "https://gql.twitch.tv/gql");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "anhelo-twitch/1.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);

#ifdef MINIMAL_MEMORY_BUFFERS
    // Add memory-saving curl options
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 16384); // Smaller receive buffer
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // Use compression
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0); // Simpler protocol
    curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 1); // Limit connections
    curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 1L); // Don't reuse connections
#endif
    
    CURLcode res = curl_easy_perform(c);
    long code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || code != 200 || !m.data) {
        if (m.data) free(m.data);
        return NULL;
    }

    
    char *sig = NULL;
    char *token = NULL;
    
    sig = extract_quoted_after(m.data, "streamPlaybackAccessToken", "\"signature\"");
    token = extract_quoted_after(m.data, "streamPlaybackAccessToken", "\"value\"");
    if (!sig || !token) {
        
        if (sig) { free(sig); sig = NULL; }
        if (token) { free(token); token = NULL; }
        sig = extract_quoted_after(m.data, "videoPlaybackAccessToken", "\"signature\"");
        token = extract_quoted_after(m.data, "videoPlaybackAccessToken", "\"value\"");
    }
    if (!sig || !token) {
        
        if (sig) { free(sig); sig = NULL; }
        if (token) { free(token); token = NULL; }
        sig = extract_quoted_after(m.data, NULL, "\"signature\"");
        token = extract_quoted_after(m.data, NULL, "\"value\"");
    }

    free(m.data);
    if (!sig || !token) { free(sig); free(token); return NULL; }

    
    c = curl_easy_init();
    char *token_e = NULL;
    int token_e_from_curl = 0;
    if (c) {
        token_e = curl_easy_escape(c, token, 0);
        if (token_e) token_e_from_curl = 1;
        curl_easy_cleanup(c);
    }
    if (!token_e) {
        token_e = strdup(token);
        token_e_from_curl = 0;
    }

    
    char master[4096];
    snprintf(master, sizeof(master), "https://usher.ttvnw.net/api/channel/hls/%s.m3u8?player=twitchweb&token=%s&sig=%s&allow_source=true&allow_audio_only=true&type=any&p=0",
        channel, token_e, sig);

    
    char *master_content = fetch_url_content(master, 5L);
    char *final_url = NULL;
    if (master_content) {
        char *variant = pick_lowest_variant_from_master(master_content, master);
        if (variant) final_url = variant;
        else final_url = strdup(master);
        free(master_content);
    } else {
        final_url = strdup(master);
    }

    if (token_e) {
        if (token_e_from_curl) curl_free(token_e);
        else free(token_e);
    }
    free(sig); free(token);
    return final_url;
}