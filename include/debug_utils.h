#ifndef DEBUG_UTILS_H
#define DEBUG_UTILS_H

#ifdef DEBUG
void print_memory_stats();
void log_memory_usage(const char* context);
#else
#define print_memory_stats() do {} while(0)
#define log_memory_usage(context) do {} while(0)
#endif

#endif // DEBUG_UTILS_H
