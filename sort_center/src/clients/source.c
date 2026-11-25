#define _POSIX_C_SOURCE 200809L
#include "tcp.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pause_arrival(int fd, uint64_t ms) {
    // Если сервер завершится во время паузы, poll заметит закрытие сокета.
    struct pollfd socket = {.fd = fd, .events = POLLIN};
    while (ms) {
        int part = ms > INT_MAX ? INT_MAX : (int)ms;
        int result = poll(&socket, 1, part);
        if (result < 0 && errno == EINTR) continue;
        if (result) return result;
        ms -= (unsigned)part;
    }
    return 0;
}

static int run_source(FILE *input, unsigned line) {
    int fd = fileno(input);
    char buffer[128], command[32];
    int length = snprintf(command, sizeof(command), "SOURCE %u\n", line);
    if (tcp_write(fd, command, (size_t)length) != 0 || !fgets(buffer, sizeof(buffer), input))
        return 1;
    unsigned destinations, min, max, delay;
    unsigned long long seed_value;
    if (sscanf(buffer, "READY %3u %7u %7u %5u %10llu", &destinations, &min, &max, &delay, &seed_value) != 5 ||
        !destinations || destinations > 100 || !min || max < min || max > 1000000 ||
        !delay || delay > 60000 || seed_value > UINT32_MAX) {
        dprintf(STDERR_FILENO, "Сервер не принял источник: %s", buffer);
        return 1;
    }
    unsigned seed = (unsigned)seed_value;
    dprintf(STDOUT_FILENO, "Источник линии %u подключён\n", line);
    for (;;) {
        unsigned interval = min + (unsigned)rand_r(&seed) % (max - min + 1);
        int ready = pause_arrival(fd, (uint64_t)delay * interval);
        if (ready < 0) return 1;
        if (!ready) {
            unsigned direction = 1 + (unsigned)rand_r(&seed) % destinations;
            length = snprintf(command, sizeof(command), "ADD %u\n", direction);
            if (tcp_write(fd, command, (size_t)length) != 0 && errno != EPIPE && errno != ECONNRESET)
                return 1;
        }

        if (!fgets(buffer, sizeof(buffer), input)) {
            if (ferror(input)) return 1;
            dprintf(STDOUT_FILENO, "Линия %u: соединение закрыто\n", line);
            return 0;
        }
        if (strcmp(buffer, "DONE\n") == 0) {
            dprintf(STDOUT_FILENO, "Линия %u: подача завершена\n", line);
            return 0;
        }
        unsigned long long id;
        if (sscanf(buffer, "ACCEPT %10llu", &id) != 1 || !id || id > UINT32_MAX) {
            dprintf(STDERR_FILENO, "Линия %u: %s", line, buffer);
            return 1;
        }
        dprintf(STDOUT_FILENO, "Линия %u: сервер принял посылку #%03llu\n", line, id);
    }
}

int main(int argc, char *argv[]) {
    char *end;
    unsigned long line = argc == 4 ? strtoul(argv[3], &end, 10) : 0;
    if (argc != 4 || *argv[3] < '0' || *argv[3] > '9' || *end || !line || line > 100) {
        dprintf(STDERR_FILENO, "Использование: %s IP PORT LINE (нумерация с 1)\n", argv[0]);
        return 1;
    }
    int fd = tcp_connect(argv[1], argv[2]);
    if (fd < 0) {
        perror("Подключение источника");
        return 1;
    }
    FILE *input = fdopen(fd, "r");
    if (!input) {
        perror("fdopen");
        close(fd);
        return 1;
    }
    setvbuf(input, NULL, _IONBF, 0);
    int result = run_source(input, (unsigned)line);
    fclose(input);
    return result;
}
