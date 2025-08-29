/* Minimal twitch resolver: resolves a channel name or twitch URL to an HLS (m3u8) URL.
 * Implementation uses `curl` via popen to call the undocumented Twitch access_token endpoint.
 * Returns a heap-allocated string that must be freed by the caller, or NULL on failure.
 */
#ifndef ANHELO_TWITCH_H
#define ANHELO_TWITCH_H

char *twitch_resolve(const char *input);

#endif
