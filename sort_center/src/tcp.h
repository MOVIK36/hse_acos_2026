#ifndef TCP_H
#define TCP_H

#include <netinet/in.h>
#include <stddef.h>

int tcp_address(const char *ip, const char *port, struct sockaddr_in *address);
int tcp_connect(const char *ip, const char *port);
int tcp_write(int fd, const char *data, size_t size);

#endif
