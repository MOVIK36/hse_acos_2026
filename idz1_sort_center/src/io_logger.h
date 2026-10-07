#ifndef IO_LOGGER_H
#define IO_LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

// Логгер с прямым выводом через дескрипторы файлов
int logger_init(const char *log_filepath, bool verbose_stdout);
void logger_close(void);

int logger_get_log_fd(void);
bool logger_is_stdout_enabled(void);

// Запись события с префиксом такта в STDOUT_FILENO и log_fd
void log_event(uint32_t tick, const char *event_tag, const char *fmt, ...);

// Прямой вывод в дескриптор без префиксов
void log_raw(int fd, const char *fmt, ...);

#endif /* IO_LOGGER_H */