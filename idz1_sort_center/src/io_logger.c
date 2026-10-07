#define _POSIX_C_SOURCE 200809L
#include "io_logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static int g_log_fd = -1;
static bool g_verbose_stdout = true;

int logger_init(const char *log_filepath, bool verbose_stdout) {
    g_verbose_stdout = verbose_stdout;
    
    if (log_filepath != NULL && strlen(log_filepath) > 0) {
        g_log_fd = open(log_filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_log_fd < 0) {
            dprintf(STDERR_FILENO, "[ERROR] Не удалось открыть лог-файл %s: %s\n",
                    log_filepath, strerror(errno));
            return -1;
        }
    }
    return 0;
}

void logger_close(void) {
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

int logger_get_log_fd(void) {
    return g_log_fd;
}

bool logger_is_stdout_enabled(void) {
    return g_verbose_stdout;
}

void log_event(uint32_t tick, const char *event_tag, const char *fmt, ...) {
    char message_buf[512];
    va_list args;
    
    va_start(args, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, args);
    va_end(args);

    // Вывод в терминал через STDOUT_FILENO
    if (g_verbose_stdout) {
        dprintf(STDOUT_FILENO, "[TICK %04u] [%-16s] %s\n", tick, event_tag, message_buf);
    }

    // Дублирование в файл лога
    if (g_log_fd >= 0) {
        dprintf(g_log_fd, "[TICK %04u] [%-16s] %s\n", tick, event_tag, message_buf);
    }
}

void log_raw(int fd, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vdprintf(fd, fmt, args);
    va_end(args);
}