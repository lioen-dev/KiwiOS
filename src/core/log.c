#include "core/log.h"
#include "libc/stdio.h"

static void log_with_level(const char *level, const char *component, const char *message) {
    kprintf("[%s] [%s] %s\n", level, component, message);
}

void log_info(const char *component, const char *message) {
    log_with_level("INFO", component, message);
}

void log_ok(const char *component, const char *message) {
    log_with_level(" OK ", component, message);
}

void log_error(const char *component, const char *message) {
    log_with_level("ERR ", component, message);
}
