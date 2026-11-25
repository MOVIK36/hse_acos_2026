#define _GNU_SOURCE
#include "tcp_server.h"
#include "io_logger.h"
#include "tcp.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLIENTS (MAX_LINES + 32)

typedef struct {
    int fd;
    unsigned line; // Номер входной линии с 1, 0 до регистрации.
    bool observer, pending, waiting, closing;
    unsigned destination;
    char input[64], reply[96];
    size_t used, sent;
    off_t log_offset;
} client_t;

struct tcp_server {
    int listener;
    unsigned sources;
    client_t clients[MAX_CLIENTS];
};

tcp_server_t *tcp_server_open(const sim_config_t *cfg) {
    struct sockaddr_in address;
    if (tcp_address(cfg->address, cfg->port, &address) != 0) return NULL;
    tcp_server_t *server = calloc(1, sizeof(*server));
    if (!server) return NULL;
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) server->clients[i].fd = -1;
    server->listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int reuse = 1;
    if (server->listener < 0 ||
        setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(server->listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener, MAX_CLIENTS) != 0) {
        int saved_errno = errno;
        tcp_server_close(server);
        errno = saved_errno;
        return NULL;
    }
    return server;
}

void tcp_server_close(tcp_server_t *server) {
    if (!server) return;
    for (unsigned i = 0; i < MAX_CLIENTS; ++i)
        if (server->clients[i].fd >= 0) close(server->clients[i].fd);
    if (server->listener >= 0) close(server->listener);
    free(server);
}

static bool enough(const sort_center_t *c) {
    return !c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels;
}

static void reply(client_t *client, const char *message) {
    strcpy(client->reply, message);
    client->sent = 0;
}

// Неблокирующая отправка. Остаток ответа передаётся на следующем шаге.
static int send_reply(client_t *client) {
    size_t remaining = strlen(client->reply) - client->sent;
    if (!remaining) return 0;
    ssize_t sent = send(client->fd, client->reply + client->sent, remaining, MSG_NOSIGNAL);
    if (sent < 0) return errno == EAGAIN || errno == EINTR ? 1 : -1;
    client->sent += (size_t)sent;
    if ((size_t)sent < remaining) return 1;
    reply(client, "");
    return 0;
}

static bool read_number(const char *command, const char *prefix, unsigned limit, unsigned *number) {
    size_t length = strlen(prefix);
    if (strncmp(command, prefix, length) != 0) return false;
    const char *value = command + length;
    char *end;
    unsigned long parsed = strtoul(value, &end, 10);
    if (*value < '0' || *value > '9' || *end || parsed == 0 || parsed > limit) return false;
    *number = (unsigned)parsed;
    return true;
}

static void handle_command(tcp_server_t *server, client_t *client, const sim_config_t *cfg,
                           const char *command, uint32_t now) {
    unsigned number;
    if (!client->line && strcmp(command, "WATCH") == 0) {
        client->observer = true;
        return;
    }
    if (!client->line && read_number(command, "SOURCE ", cfg->num_input_lines, &number)) {
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            if (server->clients[i].fd < 0 || server->clients[i].line != number) continue;
            reply(client, "ERROR Входная линия уже занята\n");
            client->closing = true;
            return;
        }
        client->line = number;
        ++server->sources;
        log_event(now, "SOURCE_CONNECT", "Подключён источник линии %u", number);
        snprintf(client->reply, sizeof(client->reply), "READY %u %u %u %u %u\n",
                 cfg->num_destinations, cfg->arrival_interval_min, cfg->arrival_interval_max,
                 cfg->step_delay_ms ? cfg->step_delay_ms : 1, cfg->random_seed + number - 1);
    } else if (client->line && read_number(command, "ADD ", cfg->num_destinations, &number)) {
        client->pending = true;
        client->waiting = false;
        client->destination = number - 1;
    } else {
        reply(client, "ERROR Неверная команда или номер линии/направления\n");
        client->closing = true;
    }
}

static int accept_parcel(client_t *client, sort_center_t *c, uint32_t now) {
    if (enough(c)) {
        reply(client, "DONE\n");
        client->pending = false;
        client->closing = true;
        return 0;
    }
    if (c->total_arrived == UINT32_MAX) return -1;
    ipc_sem_t *input = &c->inputs[client->line - 1].free_slots;
    if (sem_trywait(ipc_sem(input)) != 0) {
        if (errno != EAGAIN && errno != EINTR) return -1;
        if (!client->waiting) {
            client->waiting = true;
            ++c->total_waits;
            log_event(now, "INPUT_STALL", "Линия %u ожидает свободного входного накопителя", client->line);
        }
        return 0;
    }
    if (sem_trywait(ipc_sem(&c->parcel_free)) != 0) {
        int error = errno;
        sem_post(ipc_sem(input));
        return error == EAGAIN || error == EINTR ? 0 : -1;
    }
    uint32_t id = request_parcel(c, client->line - 1, client->destination, now);
    if (!id) {
        sem_post(ipc_sem(input));
        sem_post(ipc_sem(&c->parcel_free));
        return -1;
    }
    snprintf(client->reply, sizeof(client->reply), "ACCEPT %u\n", id);
    client->pending = false;
    return 0;
}

static int read_command(tcp_server_t *server, client_t *client, const sim_config_t *cfg, uint32_t now) {
    ssize_t read_size = recv(client->fd, client->input + client->used,
                             sizeof(client->input) - client->used, 0);
    if (read_size == 0) return -1;
    if (read_size < 0 && errno != EAGAIN && errno != EINTR) return -1;
    if (read_size > 0) client->used += (size_t)read_size;
    char *newline = memchr(client->input, '\n', client->used);
    if (client->pending && !client->used) return 0;
    if (client->pending || (!newline && client->used == sizeof(client->input)) ||
        (newline && memchr(client->input, '\0', (size_t)(newline - client->input)))) {
        reply(client, "ERROR Неверный формат запроса\n");
        client->pending = false;
        client->closing = true;
    } else if (newline) {
        *newline = '\0';
        handle_command(server, client, cfg, client->input, now);
        size_t length = (size_t)(newline + 1 - client->input);
        client->used -= length;
        memmove(client->input, newline + 1, client->used);
    }
    return 0;
}

// Журнал уже содержит события всех процессов. У наблюдателей независимые позиции чтения.
static int send_log(client_t *client) {
    char buffer[8192];
    ssize_t count = pread(logger_get_log_fd(), buffer, sizeof(buffer), client->log_offset);
    if (count < 0) return errno == EINTR ? 1 : -1;
    if (!count) return 0;
    ssize_t sent = send(client->fd, buffer, (size_t)count, MSG_NOSIGNAL);
    if (sent < 0) return errno == EAGAIN || errno == EINTR ? 1 : -1;
    client->log_offset += sent;
    return 1;
}

static void close_client(tcp_server_t *server, client_t *client) {
    if (client->line) --server->sources;
    close(client->fd);
    client->fd = -1;
}

int tcp_server_step(tcp_server_t *server, sort_center_t *c, uint32_t now) {
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
        client_t *client = &server->clients[i];
        if (client->fd >= 0) continue;
        int fd = accept4(server->listener, NULL, NULL, SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED) break;
            return -1;
        }
        *client = (client_t){.fd = fd};
    }

    // Каждый клиент выполняет короткий шаг. Никто не ждёт чужой сокет.
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
        client_t *client = &server->clients[i];
        if (client->fd < 0) continue;
        int result;
        if (client->observer) {
            char byte;
            ssize_t seen = recv(client->fd, &byte, 1, 0);
            if (seen < 0 && (errno == EAGAIN || errno == EINTR) && send_log(client) >= 0)
                continue;
        } else {
            result = send_reply(client);
            if (result > 0) continue;
            if (!result && !client->closing && read_command(server, client, &c->cfg, now) == 0) continue;
        }
        unsigned line = client->line;
        close_client(server, client);
        if (line && !enough(c)) {
            log_event(now, "SOURCE_DISCONNECT", "Источник линии %u отключился до конца подачи", line);
            return -1;
        }
    }
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
        client_t *client = &server->clients[i];
        if (client->fd < 0 || !client->pending || client->reply[0]) continue;
        if (server->sources < c->cfg.num_input_lines && !enough(c)) continue;
        center_lock(c);
        int result = c->stop ? 0 : accept_parcel(client, c, now);
        center_unlock(c);
        if (result != 0) return -1;
    }
    return 0;
}

void tcp_server_finish(tcp_server_t *server, bool failed) {
    // Даём клиентам прочитать остаток журнала, включая итоговую статистику.
    // Медленный получатель может задержать завершение максимум примерно на секунду.
    for (unsigned step = 0; step < 500; ++step) {
        bool pending = false;
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            client_t *client = &server->clients[i];
            if (client->fd < 0) continue;
            int result = client->observer ? send_log(client) : send_reply(client);
            if (result > 0) {
                pending = true;
                continue;
            }
            if (result == 0 && client->line && !client->closing) {
                reply(client, failed ? "ERROR Работа центра остановлена с ошибкой\n" : "DONE\n");
                client->closing = true;
                pending = true;
                continue;
            }
            close_client(server, client);
        }
        if (!pending) break;
        struct timespec pause = {.tv_nsec = 2000000L};
        nanosleep(&pause, NULL);
    }
}
