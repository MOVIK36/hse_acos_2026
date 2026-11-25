#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "center.h"

typedef struct tcp_server tcp_server_t;

tcp_server_t *tcp_server_open(const sim_config_t *cfg);
int tcp_server_step(tcp_server_t *server, sort_center_t *c, uint32_t now);
void tcp_server_finish(tcp_server_t *server, bool failed);
void tcp_server_close(tcp_server_t *server);

#endif
