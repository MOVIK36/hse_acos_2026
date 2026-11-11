#define _POSIX_C_SOURCE 200809L
#include "process_sim.h"
#include "center.h"
#include "io_logger.h"
#include <errno.h>
#include <signal.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>

static volatile sig_atomic_t stop_received;

static void signal_handler(int signo) {
    (void)signo;
    stop_received = 1;
}

void install_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
}

typedef struct {
    sort_center_t *center;
    uint32_t index;
} worker_arg_t;

static uint32_t tick(const sort_center_t *c) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ms = (now.tv_sec - c->started_at.tv_sec) * 1000 +
                 (now.tv_nsec - c->started_at.tv_nsec) / 1000000;
    uint32_t tick_ms = c->cfg.step_delay_ms ? c->cfg.step_delay_ms : 1;
    return (uint32_t)(ms / tick_ms);
}

static bool stopped(sort_center_t *c) {
    center_lock(c);
    if (stop_received) c->stop = true;
    bool result = c->stop;
    center_unlock(c);
    return result;
}

static void fail(sort_center_t *c, const char *message) {
    center_lock(c);
    c->failed = c->stop = true;
    center_unlock(c);
    log_event(tick(c), "ERROR", "%s", message);
}

static bool pause_ticks(sort_center_t *c, uint32_t ticks) {
    uint64_t ms = (uint64_t)ticks * (c->cfg.step_delay_ms ? c->cfg.step_delay_ms : 1);
    while (ms && !stopped(c)) {
        uint32_t part = ms > 10 ? 10 : (uint32_t)ms;
        struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)part * 1000000L};
        nanosleep(&delay, NULL);
        ms -= part;
    }
    return !stopped(c);
}

// Периодически проверяем остановку, в том числе при пустой/полной очереди.
static int timed_wait(ipc_sem_t *sem) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 50000000L;
    if (until.tv_nsec >= 1000000000L) {
        ++until.tv_sec;
        until.tv_nsec -= 1000000000L;
    }
    return sem_timedwait(ipc_sem(sem), &until);
}

static bool wait_sem(sort_center_t *c, ipc_sem_t *sem) {
    while (!stopped(c)) {
        if (timed_wait(sem) == 0) return true;
        if (errno != EINTR && errno != ETIMEDOUT) {
            fail(c, "Ошибка ожидания семафора");
            return false;
        }
    }
    return false;
}

static bool reserve_slot(sort_center_t *c, ipc_sem_t *sem, const char *tag, uint32_t id) {
    if (sem_trywait(ipc_sem(sem)) == 0) return true;
    if (errno != EAGAIN && errno != EINTR) {
        fail(c, "Ошибка семафора свободных мест");
        return false;
    }
    center_lock(c);
    ++c->total_waits;
    if (strcmp(tag, "INPUT_STALL") == 0)
        log_event(tick(c), tag, "Линия %u ожидает свободного входного накопителя", id);
    else
        log_event(tick(c), tag, "Посылка #%03u ожидает свободного места", id);
    center_unlock(c);
    return wait_sem(c, sem);
}

// Один процесс на посылку. Родитель освобождает её ячейку после waitpid.
static void parcel_process(sort_center_t *c, unsigned slot) {
    parcel_job_t *p = &c->parcels[slot];
    fifo_queue_t *q = &c->inputs[p->data.source_line];
    center_lock(c);
    if (!c->stop) {
        p->data.stage = STAGE_INPUT_BUFFER;
        fifo_push(q, c, p);
        log_event(tick(c), "INPUT_ENQUEUE", "Посылка #%03u поставлена во вход %u (%u/%u)",
                  p->data.id, p->data.source_line + 1, q->count, q->capacity);
        sem_post(ipc_sem(&c->scan_jobs));
    } else sem_post(ipc_sem(&q->free_slots));
    center_unlock(c);
    bool completed = wait_sem(c, &p->done);
    center_lock(c);
    log_event(tick(c), "PARCEL_END", "Процесс посылки #%03u завершён: %s",
              p->data.id, completed ? parcel_stage_to_string(p->data.stage) : "остановка");
    center_unlock(c);
}

static void source_process(worker_arg_t *a) {
    sort_center_t *c = a->center;
    unsigned seed = c->cfg.random_seed + a->index;
    fifo_queue_t *q = &c->inputs[a->index];
    while (!stopped(c)) {
        center_lock(c);
        bool enough = !c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels;
        center_unlock(c);
        if (enough) break;
        uint32_t interval = c->cfg.arrival_interval_min +
            (uint32_t)rand_r(&seed) % (c->cfg.arrival_interval_max - c->cfg.arrival_interval_min + 1);
        if (!pause_ticks(c, interval) || !reserve_slot(c, &q->free_slots, "INPUT_STALL", a->index + 1)) break;
        if (!wait_sem(c, &c->parcel_free)) {
            sem_post(ipc_sem(&q->free_slots));
            break;
        }
        center_lock(c);
        enough = !c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels;
        if (c->stop || enough || c->total_arrived == UINT32_MAX) {
            bool exhausted = c->total_arrived == UINT32_MAX;
            center_unlock(c);
            sem_post(ipc_sem(&q->free_slots));
            sem_post(ipc_sem(&c->parcel_free));
            if (exhausted) fail(c, "Исчерпан диапазон идентификаторов");
            break;
        }
        unsigned slot = 0;
        while (slot < MAX_PARCELS && c->parcels[slot].state) ++slot;
        if (slot == MAX_PARCELS) {
            center_unlock(c);
            fail(c, "Нарушен баланс ячеек посылок");
            break;
        }
        parcel_job_t *p = &c->parcels[slot];
        p->data = (parcel_t){.id = ++c->total_arrived, .source_line = a->index,
            .target_destination = (uint32_t)rand_r(&seed) % c->cfg.num_destinations,
            .arrival_tick = tick(c)};
        p->state = 1;
        log_event(tick(c), "PARCEL_ARRIVAL", "Посылка #%03u: линия %u, направление %u, запрос на создание процесса",
                  p->data.id, a->index + 1, p->data.target_destination + 1);
        center_unlock(c);
    }
    return;
}

static void scanner_process(worker_arg_t *a) {
    sort_center_t *c = a->center;
    unsigned seed = c->cfg.random_seed + 1000 + a->index;
    while (wait_sem(c, &c->scan_jobs)) {
        center_lock(c);
        if (c->stop) {
            center_unlock(c);
            break;
        }
        parcel_job_t *p = NULL;
        for (uint32_t i = 0; i < c->cfg.num_input_lines; ++i) {
            uint32_t line = (c->next_input_line + i) % c->cfg.num_input_lines;
            if (c->inputs[line].count) {
                p = fifo_pop(&c->inputs[line], c);
                sem_post(ipc_sem(&c->inputs[line].free_slots));
                c->next_input_line = (line + 1) % c->cfg.num_input_lines;
                break;
            }
        }
        center_unlock(c);
        if (!p) {
            fail(c, "Нарушен баланс входных очередей");
            break;
        }
        bool read_ok = false;
        for (;;) {
            p->data.stage = STAGE_SCANNING;
            log_event(tick(c), "SCAN_START", "Сканер %u читает посылку #%03u (попытка %u)",
                      a->index + 1, p->data.id, p->data.rescan_attempts + 1);
            if (!pause_ticks(c, c->cfg.scan_duration)) break;
            read_ok = (uint32_t)rand_r(&seed) % 100 >= c->cfg.label_error_prob_pct;
            p->data.label_stat = read_ok ? LABEL_OK : LABEL_UNREADABLE;
            log_event(tick(c), "SCAN_FINISH", "Сканер %u, посылка #%03u: %s", a->index + 1,
                      p->data.id, read_ok ? "код прочитан" : "ошибка чтения");
            if (read_ok || p->data.rescan_attempts == c->cfg.max_rescan_attempts) break;
            ++p->data.rescan_attempts;
            center_lock(c);
            ++c->total_rescans;
            center_unlock(c);
            log_event(tick(c), "SCAN_RETRY", "Посылка #%03u: повторное чтение %u/%u тем же сканером",
                      p->data.id, p->data.rescan_attempts, c->cfg.max_rescan_attempts);
        }
        if (stopped(c)) break;
        if (read_ok) {
            p->data.destination_recognized = true;
            p->data.label_stat = LABEL_OK;
            log_event(tick(c), "SCAN_SUCCESS", "Посылка #%03u: определено направление %u",
                      p->data.id, p->data.target_destination + 1);
            fifo_queue_t *q = &c->intermediate;
            if (!reserve_slot(c, &q->free_slots, "SCAN_BUFFER_STALL", p->data.id)) break;
            center_lock(c);
            p->data.stage = STAGE_POST_SCAN_BUFFER;
            fifo_push(q, c, p);
            log_event(tick(c), "SCAN_TRANSFER", "Посылка #%03u: в промежуточный буфер (%u/%u)",
                      p->data.id, q->count, q->capacity);
            sem_post(ipc_sem(&c->sort_jobs));
            center_unlock(c);
        } else {
            if (!reserve_slot(c, &c->manual_free, "MANUAL_STALL", p->data.id)) break;
            center_lock(c);
            p->data.label_stat = LABEL_DAMAGED;
            p->data.stage = STAGE_MANUAL_INSPECTION;
            p->data.completion_tick = tick(c);
            ++c->manual_count;
            ++c->total_manual_rejected;
            log_event(tick(c), "MANUAL_REJECT", "Посылка #%03u: ручная зона (%u/%u)",
                      p->data.id, c->manual_count, c->cfg.manual_buffer_cap);
            sem_post(ipc_sem(&p->done));
            center_unlock(c);
        }
    }
    return;
}

static void sorter_process(worker_arg_t *a) {
    sort_center_t *c = a->center;
    fifo_queue_t *q = &c->intermediate;
    while (wait_sem(c, &c->sort_jobs)) {
        center_lock(c);
        if (c->stop) {
            center_unlock(c);
            break;
        }
        parcel_job_t *p = fifo_pop(q, c);
        sem_post(ipc_sem(&q->free_slots));
        p->data.stage = STAGE_SORTING;
        log_event(tick(c), "SORT_START", "Сортировщик %u принял посылку #%03u", a->index + 1, p->data.id);
        center_unlock(c);
        if (!pause_ticks(c, c->cfg.sort_duration)) break;
        conveyor_line_t *line = &c->conveyors[p->data.target_destination];
        if (!reserve_slot(c, &line->entry_free, "CONVEYOR_STALL", p->data.id)) break;
        center_lock(c);
        p->data.stage = STAGE_CONVEYOR;
        line->cells[0] = (uint32_t)(p - c->parcels) + 1;
        log_event(tick(c), "SORT_DISPATCH", "Посылка #%03u: на конвейер %u", p->data.id, p->data.target_destination + 1);
        center_unlock(c);
    }
    return;
}

static void conveyor_process(worker_arg_t *a) {
    sort_center_t *c = a->center;
    conveyor_line_t *line = &c->conveyors[a->index];
    while (pause_ticks(c, 1)) {
        center_lock(c);
        bool waited = false;
        for (uint32_t step = 0; step < c->cfg.conveyor_speed; ++step) {
            uint32_t last = c->cfg.conveyor_length - 1;
            parcel_job_t *p = line->cells[last] ? &c->parcels[line->cells[last] - 1] : NULL;
            if (p && sem_trywait(ipc_sem(&line->output_free)) == 0) {
                line->cells[last] = 0;
                if (!last) sem_post(ipc_sem(&line->entry_free));
                p->data.stage = STAGE_OUTPUT_BIN;
                p->data.completion_tick = tick(c);
                ++line->output_count;
                ++line->delivered;
                ++c->total_delivered;
                c->sum_transit_ticks += p->data.completion_tick - p->data.arrival_tick;
                log_event(tick(c), "OUTPUT_DELIVERED", "Посылка #%03u: выход %u (%u/%u)",
                          p->data.id, a->index + 1, line->output_count, c->cfg.output_bin_cap);
                sem_post(ipc_sem(&p->done));
            } else if (p && !waited) {
                ++c->total_waits;
                waited = true;
                log_event(tick(c), "OUTPUT_STALL", "Посылка #%03u: выход %u полон", p->data.id, a->index + 1);
            }
            for (uint32_t i = last; i > 0; --i) {
                if (!line->cells[i] && line->cells[i - 1]) {
                    line->cells[i] = line->cells[i - 1];
                    line->cells[i - 1] = 0;
                    if (i == 1) sem_post(ipc_sem(&line->entry_free));
                }
            }
        }
        center_unlock(c);
    }
    return;
}

static void service_process(worker_arg_t *a) {
    sort_center_t *c = a->center;
    uint32_t next_output = c->cfg.output_dispatch_interval;
    uint32_t next_manual = c->cfg.manual_service_interval;
    while (pause_ticks(c, 1)) {
        uint32_t now = tick(c);
        center_lock(c);
        if (now >= next_output) {
            for (uint32_t d = 0; d < c->cfg.num_destinations; ++d) {
                conveyor_line_t *line = &c->conveyors[d];
                uint32_t count = line->output_count;
                line->output_count = 0;
                c->total_shipped += count;
                for (uint32_t i = 0; i < count; ++i) sem_post(ipc_sem(&line->output_free));
                if (count) log_event(now, "OUTPUT_SHIPPED", "Из выхода %u отгружено %u посылок", d + 1, count);
            }
            next_output = now + c->cfg.output_dispatch_interval;
        }
        if (now >= next_manual) {
            uint32_t count = c->manual_count;
            c->manual_count = 0;
            c->total_manual_serviced += count;
            for (uint32_t i = 0; i < count; ++i) sem_post(ipc_sem(&c->manual_free));
            if (count) log_event(now, "MANUAL_SERVICED", "Оператору передано %u посылок", count);
            next_manual = now + c->cfg.manual_service_interval;
        }
        center_unlock(c);
    }
    return;
}

static void print_summary(const sort_center_t *c, int fd, bool interrupted) {
    dprintf(fd, "\n==================== ИТОГОВАЯ СТАТИСТИКА ====================\n");
    dprintf(fd, "  Статус: %s\n", c->failed ? "ОШИБКА" : interrupted ? "ПРЕРВАНО ПО ЗАПРОСУ" : "ЗАВЕРШЕНО УСПЕШНО");
    dprintf(fd, "  Прибыло посылок: %u\n  Доставлено в накопители: %u\n  Отбраковано (ручная зона): %u\n",
            c->total_arrived, c->total_delivered, c->total_manual_rejected);
    dprintf(fd, "  Повторных сканирований: %" PRIu64 "\n  Событий ожидания: %" PRIu64 "\n", c->total_rescans, c->total_waits);
    if (c->total_delivered) dprintf(fd, "  Среднее время доставки: %.2f тактов\n", (double)c->sum_transit_ticks / c->total_delivered);
    dprintf(fd, "  Отгружено с выходов: %u; передано оператору: %u\n", c->total_shipped, c->total_manual_serviced);
    for (uint32_t d = 0; d < c->cfg.num_destinations; ++d)
        dprintf(fd, "  Направление %u: доставлено %u, в накопителе %u/%u\n", d + 1,
                c->conveyors[d].delivered, c->conveyors[d].output_count, c->cfg.output_bin_cap);
    dprintf(fd, "  Ручная зона: %u/%u\n", c->manual_count, c->cfg.manual_buffer_cap);
    dprintf(fd, "  Незавершённых посылок: %u\n", c->total_arrived - c->total_delivered - c->total_manual_rejected);
}

// Порядок ролей одинаков для дочерних и независимо запущенных процессов.
static unsigned worker_count(const sim_config_t *cfg) {
    return cfg->num_scanners + cfg->num_sorters + cfg->num_destinations +
           cfg->num_input_lines + 1;
}

static int worker_number(const sim_config_t *cfg, const char *role, unsigned index) {
    if (strcmp(role, "scanner") == 0 && index < cfg->num_scanners) return (int)index;
    unsigned offset = cfg->num_scanners;
    if (strcmp(role, "sorter") == 0 && index < cfg->num_sorters) return (int)(offset + index);
    offset += cfg->num_sorters;
    if (strcmp(role, "conveyor") == 0 && index < cfg->num_destinations) return (int)(offset + index);
    offset += cfg->num_destinations;
    if (strcmp(role, "service") == 0 && index == 0) return (int)offset;
    ++offset;
    if (strcmp(role, "source") == 0 && index < cfg->num_input_lines) return (int)(offset + index);
    return -1;
}

static void worker_process(sort_center_t *c, unsigned number) {
    worker_arg_t a = {.center = c};
    unsigned n = number;
    if (n < c->cfg.num_scanners) { a.index = n; scanner_process(&a); }
    else if ((n -= c->cfg.num_scanners) < c->cfg.num_sorters) { a.index = n; sorter_process(&a); }
    else if ((n -= c->cfg.num_sorters) < c->cfg.num_destinations) { a.index = n; conveyor_process(&a); }
    else if ((n -= c->cfg.num_destinations) == 0) service_process(&a);
    else { a.index = n - 1; source_process(&a); }
    center_lock(c);
    c->workers[number].done = true;
    log_event(tick(c), "WORKER_END", "Участник %u, PID %ld завершён", number, (long)getpid());
    center_unlock(c);
}

static bool register_worker(sort_center_t *c, unsigned number) {
    center_lock(c);
    if (c->stop || c->workers[number].pid) {
        center_unlock(c);
        return false;
    }
    c->workers[number].pid = getpid();
    ++c->registered;
    log_event(tick(c), "WORKER_START", "Участник %u, PID %ld подключён", number, (long)getpid());
    center_unlock(c);
    return true;
}

static void short_pause(void) {
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 2000000L};
    nanosleep(&delay, NULL);
}

int run_worker(const char *session, const char *role, const char *index_text) {
    if (!*session || strlen(session) >= 48) return 1;
    for (const char *p = session; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_') return 1;
    if (!*index_text || !isdigit((unsigned char)*index_text)) return 1;
    char *end;
    errno = 0;
    unsigned long index = strtoul(index_text, &end, 10);
    if (*end || errno || index >= MAX_WORKERS) return 1;
    sort_center_t *c = NULL;
    // Участников можно запускать сразу после координатора, пока он создаёт IPC.
    for (unsigned attempt = 0; attempt < 1500 && !stop_received; ++attempt) {
        if (center_open(session, &c) == 0) break;
        short_pause();
    }
    if (!c) {
        dprintf(STDERR_FILENO, "Не удалось подключиться к сессии %s\n", session);
        return 1;
    }
    int number = worker_number(&c->cfg, role, (unsigned)index);
    if (number < 0 || logger_attach(c->cfg.log_filepath, c->cfg.verbose_stdout) != 0) {
        center_close(c, false);
        return 1;
    }
    logger_set_semaphore(ipc_sem(&c->log_mutex));
    if (!register_worker(c, (unsigned)number)) {
        dprintf(STDERR_FILENO, "Роль уже занята или сессия завершена\n");
        logger_close();
        center_close(c, false);
        return 1;
    }
    worker_process(c, (unsigned)number);
    center_lock(c);
    int result = c->failed ? 1 : 0;
    center_unlock(c);
    logger_close();
    center_close(c, false);
    return result;
}

static bool children_finished(pid_t *pids, unsigned count, sort_center_t *c) {
    bool all = true;
    for (unsigned i = 0; i < count; ++i) {
        if (!pids[i]) continue;
        int status;
        pid_t result = waitpid(pids[i], &status, WNOHANG);
        if (result == pids[i]) {
            pids[i] = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status)) fail(c, "Дочерний процесс завершился с ошибкой");
        } else if (result < 0 && errno != EINTR) {
            pids[i] = 0;
            fail(c, "Ошибка waitpid");
        } else all = false;
    }
    return all;
}

// Проверяем независимых участников: завершение отмечается под общим семафором.
static bool workers_finished(sort_center_t *c) {
    bool all = true;
    center_lock(c);
    for (unsigned i = 0; i < worker_count(&c->cfg); ++i) {
        worker_info_t *w = &c->workers[i];
        if (!w->pid || w->done) continue;
        all = false;
        if (kill(w->pid, 0) != 0 && errno == ESRCH) {
            c->failed = c->stop = true;
            w->done = true; // Исчезнувший процесс уже не выставит эту отметку сам.
        }
    }
    center_unlock(c);
    return all;
}

int sim_run(const sim_config_t *cfg) {
    sort_center_t *c;
    if (center_create(cfg, &c) != 0) {
        dprintf(STDERR_FILENO, "Не удалось создать IPC сессии %s: %s\n", cfg->session, strerror(errno));
        return 1;
    }
    if (logger_init(cfg->log_filepath, cfg->verbose_stdout) != 0) {
        center_close(c, true);
        return 1;
    }
    logger_set_semaphore(ipc_sem(&c->log_mutex));
    config_print(cfg, STDOUT_FILENO);
    config_print(cfg, logger_get_log_fd());
    log_event(0, "SIM_START", "Сортировочный центр с процессами, режим %s", cfg->ipc_mode);
    bool independent = strcmp(cfg->ipc_mode, "independent") == 0;
    pid_t workers[MAX_WORKERS] = {0}, parcels[MAX_PARCELS] = {0};
    uint32_t joined = 0;
    bool interrupted = false;
    // Готовность публикуется только после создания всех семафоров и журнала.
    atomic_store(&c->ready, true);
    unsigned count = worker_count(cfg);
    if (!independent && (cfg->unlimited_mode || cfg->num_parcels)) {
        for (unsigned i = 0; i < count && !stopped(c); ++i) {
            pid_t pid = fork();
            if (pid == 0) {
                if (!register_worker(c, i)) _exit(1);
                worker_process(c, i);
                logger_close();
                center_close(c, false);
                _exit(0);
            }
            if (pid < 0) { fail(c, "Не удалось создать процесс устройства/источника"); break; }
            workers[i] = pid;
        }
    }
    while (!stopped(c)) {
        if (tick(c) >= UINT32_MAX - 1000001U) {
            fail(c, "Исчерпан диапазон модельного времени");
            break;
        }
        children_finished(workers, count, c);
        workers_finished(c);
        if (independent) {
            center_lock(c);
            unsigned registered = c->registered;
            center_unlock(c);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (registered < count && now.tv_sec - c->started_at.tv_sec >= 10) {
                fail(c, "За 10 секунд подключились не все участники");
                break;
            }
        }
        // Все fork выполняет один родитель, включая процессы каждой посылки.
        for (unsigned slot = 0; slot < MAX_PARCELS; ++slot) {
            center_lock(c);
            bool requested = c->parcels[slot].state == 1 && !c->stop;
            if (requested) {
                pid_t pid = fork();
                if (pid == 0) {
                    // Общий семафор освободит родитель.
                    parcel_process(c, slot);
                    logger_close();
                    center_close(c, false);
                    _exit(0);
                }
                if (pid > 0) {
                    parcels[slot] = pid;
                    c->parcels[slot].state = 2;
                    log_event(tick(c), "PARCEL_START", "Посылка #%03u: создан процесс PID %ld",
                              c->parcels[slot].data.id, (long)pid);
                }
                center_unlock(c);
                if (pid < 0) { fail(c, "Не удалось создать процесс посылки"); break; }
            } else center_unlock(c);
            if (!parcels[slot]) continue;
            int status;
            pid_t result = waitpid(parcels[slot], &status, WNOHANG);
            if (result == parcels[slot]) {
                parcels[slot] = 0;
                ++joined;
                if (!WIFEXITED(status) || WEXITSTATUS(status)) fail(c, "Ошибка процесса посылки");
                center_lock(c);
                // Уведомление уже прочитано посылкой. При остановке оно могло остаться.
                while (sem_trywait(ipc_sem(&c->parcels[slot].done)) == 0) {}
                c->parcels[slot].state = 0;
                sem_post(ipc_sem(&c->parcel_free));
                center_unlock(c);
            }
        }
        if (!cfg->unlimited_mode && joined == cfg->num_parcels) {
            center_lock(c);
            bool all_registered = !independent || c->registered == count;
            center_unlock(c);
            if (all_registered) break;
        }
        short_pause();
    }
    center_lock(c);
    interrupted = (stop_received || c->stop) && !c->failed;
    c->stop = true;
    center_unlock(c);
    // Источники и устройства должны выйти раньше, чем уничтожатся IPC-объекты.
    while (!children_finished(workers, count, c) || !workers_finished(c)) short_pause();
    for (unsigned slot = 0; slot < MAX_PARCELS; ++slot) {
        if (!parcels[slot]) continue;
        int status;
        pid_t result;
        do { result = waitpid(parcels[slot], &status, 0); } while (result < 0 && errno == EINTR);
        if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) fail(c, "Ошибка ожидания посылки");
    }
    uint64_t output_count = c->total_shipped;
    for (uint32_t d = 0; d < cfg->num_destinations; ++d) output_count += c->conveyors[d].output_count;
    if (output_count != c->total_delivered ||
        (uint64_t)c->manual_count + c->total_manual_serviced != c->total_manual_rejected ||
        (!interrupted && !c->failed && c->total_delivered + c->total_manual_rejected != c->total_arrived))
        fail(c, "Нарушен баланс посылок");
    log_event(tick(c), interrupted ? "SIM_INTERRUPT" : c->failed ? "SIM_ABORT" : "SIM_END",
              "Дочерние процессы собраны через waitpid, независимые участники закончили работу");
    print_summary(c, STDOUT_FILENO, interrupted);
    print_summary(c, logger_get_log_fd(), interrupted);
    int result = c->failed ? 1 : 0;
    center_close(c, true);
    return result;
}
