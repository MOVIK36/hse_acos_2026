#define _POSIX_C_SOURCE 200809L
#include "io_logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <semaphore.h>

static sem_t *log_mutex;

void logger_set_semaphore(sem_t *sem) { log_mutex = sem; }

static int g_log_fd = -1;
static bool g_verbose_stdout = true;

int logger_init(const char *log_filepath, bool verbose_stdout) {
    g_verbose_stdout = verbose_stdout;

    if (log_filepath != NULL && strlen(log_filepath) > 0) {
        g_log_fd = open(log_filepath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (g_log_fd < 0) {
            dprintf(STDERR_FILENO, "[ERROR] Не удалось открыть лог-файл %s: %s\n",
                    log_filepath, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int logger_attach(const char *path, bool verbose_stdout) {
    g_verbose_stdout = verbose_stdout;
    g_log_fd = open(path, O_WRONLY | O_APPEND);
    return g_log_fd < 0 ? -1 : 0;
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

void log_event(uint32_t tick, const char *event_tag, const char *fmt, ...) {
    char message_buf[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, args);
    va_end(args);

    if (log_mutex) while (sem_wait(log_mutex) != 0 && errno == EINTR) {}

    if (g_verbose_stdout) {
        dprintf(STDOUT_FILENO, "[TICK %04u] [%-16s] %s\n", tick, event_tag, message_buf);
    }

    if (g_log_fd >= 0) {
        dprintf(g_log_fd, "[TICK %04u] [%-16s] %s\n", tick, event_tag, message_buf);
    }
    if (log_mutex) sem_post(log_mutex);
}
