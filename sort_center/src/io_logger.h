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

// Запись события с префиксом такта в STDOUT_FILENO и log_fd
void log_event(uint32_t tick, const char *event_tag, const char *fmt, ...);


#endif /* IO_LOGGER_H */
