#define _POSIX_C_SOURCE 200809L
#include "tcp.h"
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        dprintf(STDERR_FILENO, "Использование: %s IP PORT\n", argv[0]);
        return 1;
    }
    int fd = tcp_connect(argv[1], argv[2]);
    if (fd < 0) {
        perror("Подключение наблюдателя");
        return 1;
    }
    if (tcp_write(fd, "WATCH\n", 6) != 0) {
        perror("Запрос наблюдателя");
        close(fd);
        return 1;
    }
    char buffer[8192];
    int result = 0;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (count < 0) {
                perror("Чтение журнала");
                result = 1;
            }
            break;
        }
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t size = write(STDOUT_FILENO, buffer + offset, (size_t)(count - offset));
            if (size < 0 && errno == EINTR) continue;
            if (size <= 0) {
                close(fd);
                return 1;
            }
            offset += size;
        }
    }
    close(fd);
    return result;
}
