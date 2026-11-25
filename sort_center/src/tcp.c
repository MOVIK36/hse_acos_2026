#define _POSIX_C_SOURCE 200809L
#include "tcp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int tcp_address(const char *ip, const char *port, struct sockaddr_in *address) {
    char *end;
    *address = (struct sockaddr_in){.sin_family = AF_INET};
    unsigned long number = strtoul(port, &end, 10);
    if (*port < '0' || *port > '9' || *end || number == 0 || number > 65535 ||
        inet_pton(AF_INET, ip, &address->sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    address->sin_port = htons((unsigned short)number);
    return 0;
}

int tcp_connect(const char *ip, const char *port) {
    struct sockaddr_in address;
    if (tcp_address(ip, port, &address) != 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) return fd;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

int tcp_write(int fd, const char *data, size_t size) {
    while (size) {
        ssize_t sent = send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        data += sent;
        size -= (size_t)sent;
    }
    return 0;
}
