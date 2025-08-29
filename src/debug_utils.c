#include "../include/debug_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEBUG
void print_memory_stats() {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) return;
    
    char line[128];
    printf("=== Memory Statistics ===\n");
    
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "VmRSS:", 6) == 0 || 
            strncmp(line, "VmSize:", 7) == 0 ||
            strncmp(line, "VmPeak:", 7) == 0 ||
            strncmp(line, "VmHWM:", 6) == 0) {
            printf("%s", line);
        }
    }
    fclose(status);
    printf("========================\n");
}

void log_memory_usage(const char* context) {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) return;
    
    char line[128];
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            printf("[%s] Memory usage: %s", context, line);
            break;
        }
    }
    fclose(status);
}
#endif
