#define _POSIX_C_SOURCE 200809L
#include "thread_sim.h"
#include "center.h"
#include "io_logger.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    pthread_mutex_lock(&c->mutex);
    bool result = c->stop;
    pthread_mutex_unlock(&c->mutex);
    return result;
}

static void fail(sort_center_t *c, const char *message) {
    pthread_mutex_lock(&c->mutex);
    c->failed = c->stop = true;
    pthread_mutex_unlock(&c->mutex);
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
static int timed_wait(sem_t *sem) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 50000000L;
    if (until.tv_nsec >= 1000000000L) {
        ++until.tv_sec;
        until.tv_nsec -= 1000000000L;
    }
    return sem_timedwait(sem, &until);
}

static bool wait_sem(sort_center_t *c, sem_t *sem) {
    while (!stopped(c)) {
        if (timed_wait(sem) == 0) return true;
        if (errno != EINTR && errno != ETIMEDOUT) {
            fail(c, "Ошибка ожидания семафора");
            return false;
        }
    }
    return false;
}

static bool reserve_slot(sort_center_t *c, sem_t *sem, const char *tag, uint32_t id) {
    if (sem_trywait(sem) == 0) return true;
    if (errno != EAGAIN && errno != EINTR) {
        fail(c, "Ошибка семафора свободных мест");
        return false;
    }
    pthread_mutex_lock(&c->mutex);
    ++c->total_waits;
    if (strcmp(tag, "INPUT_STALL") == 0)
        log_event(tick(c), tag, "Линия %u ожидает свободного входного накопителя", id);
    else
        log_event(tick(c), tag, "Посылка #%03u ожидает свободного места", id);
    pthread_mutex_unlock(&c->mutex);
    return wait_sem(c, sem);
}

static void *parcel_thread(void *arg) {
    parcel_job_t *p = arg;
    sort_center_t *c = p->center;
    fifo_queue_t *q = &c->inputs[p->data.source_line];
    pthread_mutex_lock(&c->mutex);
    if (!c->stop) {
        p->data.stage = STAGE_INPUT_BUFFER;
        fifo_push(q, p); // Место заранее зарезервировал источник.
        log_event(tick(c), "INPUT_ENQUEUE", "Посылка #%03u поставлена во вход %u (%u/%u)",
                  p->data.id, p->data.source_line + 1, q->count, q->capacity);
        sem_post(&c->scan_jobs);
    } else {
        sem_post(&q->free_slots);
    }
    pthread_mutex_unlock(&c->mutex);
    bool completed = wait_sem(c, &p->done);

    // Устройства уведомляют о результате под тем же mutex и дальше p не используют.
    pthread_mutex_lock(&c->mutex);
    log_event(tick(c), "PARCEL_END", "Поток посылки #%03u завершён: %s",
              p->data.id, completed ? parcel_stage_to_string(p->data.stage) : "остановка");
    if (c->finished_tail) c->finished_tail->next_finished = p;
    else c->finished_head = p;
    c->finished_tail = p;
    sem_post(&c->finished);
    pthread_mutex_unlock(&c->mutex);
    return NULL;
}

static void *source_thread(void *arg) {
    worker_arg_t *a = arg;
    sort_center_t *c = a->center;
    unsigned seed = c->cfg.random_seed + a->index;
    fifo_queue_t *q = &c->inputs[a->index];
    while (!stopped(c)) {
        pthread_mutex_lock(&c->mutex);
        bool enough = !c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels;
        pthread_mutex_unlock(&c->mutex);
        if (enough) break;
        uint32_t interval = c->cfg.arrival_interval_min +
            (uint32_t)rand_r(&seed) % (c->cfg.arrival_interval_max - c->cfg.arrival_interval_min + 1);
        if (!pause_ticks(c, interval) || !reserve_slot(c, &q->free_slots, "INPUT_STALL", a->index + 1)) break;
        parcel_job_t *p = calloc(1, sizeof(*p));
        if (!p) {
            sem_post(&q->free_slots);
            fail(c, "Не удалось создать посылку");
            break;
        }
        if (sem_init(&p->done, 0, 0) != 0) {
            free(p);
            sem_post(&q->free_slots);
            fail(c, "Не удалось создать семафор посылки");
            break;
        }
        p->center = c;
        p->data.source_line = a->index;
        p->data.target_destination = (uint32_t)rand_r(&seed) % c->cfg.num_destinations;
        pthread_mutex_lock(&c->mutex);
        enough = !c->cfg.unlimited_mode && c->total_arrived >= c->cfg.num_parcels;
        if (c->stop || enough) {
            pthread_mutex_unlock(&c->mutex);
            sem_destroy(&p->done);
            free(p);
            sem_post(&q->free_slots);
            break;
        }
        if (c->total_arrived == UINT32_MAX) {
            pthread_mutex_unlock(&c->mutex);
            sem_destroy(&p->done);
            free(p);
            sem_post(&q->free_slots);
            fail(c, "Исчерпан диапазон идентификаторов");
            break;
        }
        p->data.id = c->total_arrived + 1;
        p->data.arrival_tick = tick(c);
        int error = pthread_create(&p->thread, NULL, parcel_thread, p);
        if (!error) {
            ++c->total_arrived;
            log_event(tick(c), "PARCEL_ARRIVAL", "Посылка #%03u: линия %u, направление %u, создан поток",
                      p->data.id, a->index + 1, p->data.target_destination + 1);
        }
        pthread_mutex_unlock(&c->mutex);
        if (error) {
            sem_destroy(&p->done);
            free(p);
            sem_post(&q->free_slots);
            fail(c, "Не удалось создать поток посылки");
            break;
        }
    }
    return NULL;
}

static void *scanner_thread(void *arg) {
    worker_arg_t *a = arg;
    sort_center_t *c = a->center;
    unsigned seed = c->cfg.random_seed + 1000 + a->index;
    while (wait_sem(c, &c->scan_jobs)) {
        pthread_mutex_lock(&c->mutex);
        if (c->stop) {
            pthread_mutex_unlock(&c->mutex);
            break;
        }
        parcel_job_t *p = NULL;
        for (uint32_t i = 0; i < c->cfg.num_input_lines; ++i) {
            uint32_t line = (c->next_input_line + i) % c->cfg.num_input_lines;
            if (c->inputs[line].count) {
                p = fifo_pop(&c->inputs[line]);
                sem_post(&c->inputs[line].free_slots);
                c->next_input_line = (line + 1) % c->cfg.num_input_lines;
                break;
            }
        }
        pthread_mutex_unlock(&c->mutex);
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
            pthread_mutex_lock(&c->mutex);
            ++c->total_rescans;
            pthread_mutex_unlock(&c->mutex);
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
            pthread_mutex_lock(&c->mutex);
            p->data.stage = STAGE_POST_SCAN_BUFFER;
            fifo_push(q, p);
            log_event(tick(c), "SCAN_TRANSFER", "Посылка #%03u: в промежуточный буфер (%u/%u)",
                      p->data.id, q->count, q->capacity);
            sem_post(&c->sort_jobs);
            pthread_mutex_unlock(&c->mutex);
        } else {
            if (!reserve_slot(c, &c->manual_free, "MANUAL_STALL", p->data.id)) break;
            pthread_mutex_lock(&c->mutex);
            p->data.label_stat = LABEL_DAMAGED;
            p->data.stage = STAGE_MANUAL_INSPECTION;
            p->data.completion_tick = tick(c);
            ++c->manual_count;
            ++c->total_manual_rejected;
            log_event(tick(c), "MANUAL_REJECT", "Посылка #%03u: ручная зона (%u/%u)",
                      p->data.id, c->manual_count, c->cfg.manual_buffer_cap);
            sem_post(&p->done);
            pthread_mutex_unlock(&c->mutex);
        }
    }
    return NULL;
}

static void *sorter_thread(void *arg) {
    worker_arg_t *a = arg;
    sort_center_t *c = a->center;
    fifo_queue_t *q = &c->intermediate;
    while (wait_sem(c, &c->sort_jobs)) {
        pthread_mutex_lock(&c->mutex);
        if (c->stop) {
            pthread_mutex_unlock(&c->mutex);
            break;
        }
        parcel_job_t *p = fifo_pop(q);
        sem_post(&q->free_slots);
        p->data.stage = STAGE_SORTING;
        log_event(tick(c), "SORT_START", "Сортировщик %u принял посылку #%03u", a->index + 1, p->data.id);
        pthread_mutex_unlock(&c->mutex);
        if (!pause_ticks(c, c->cfg.sort_duration)) break;
        conveyor_line_t *line = &c->conveyors[p->data.target_destination];
        if (!reserve_slot(c, &line->entry_free, "CONVEYOR_STALL", p->data.id)) break;
        pthread_mutex_lock(&c->mutex);
        p->data.stage = STAGE_CONVEYOR;
        line->cells[0] = p;
        log_event(tick(c), "SORT_DISPATCH", "Посылка #%03u: на конвейер %u", p->data.id, p->data.target_destination + 1);
        pthread_mutex_unlock(&c->mutex);
    }
    return NULL;
}

static void *conveyor_thread(void *arg) {
    worker_arg_t *a = arg;
    sort_center_t *c = a->center;
    conveyor_line_t *line = &c->conveyors[a->index];
    while (pause_ticks(c, 1)) {
        pthread_mutex_lock(&c->mutex);
        bool waited = false;
        for (uint32_t step = 0; step < c->cfg.conveyor_speed; ++step) {
            uint32_t last = c->cfg.conveyor_length - 1;
            parcel_job_t *p = line->cells[last];
            if (p && sem_trywait(&line->output_free) == 0) {
                line->cells[last] = NULL;
                if (!last) sem_post(&line->entry_free);
                p->data.stage = STAGE_OUTPUT_BIN;
                p->data.completion_tick = tick(c);
                ++line->output_count;
                ++line->delivered;
                ++c->total_delivered;
                c->sum_transit_ticks += p->data.completion_tick - p->data.arrival_tick;
                log_event(tick(c), "OUTPUT_DELIVERED", "Посылка #%03u: выход %u (%u/%u)",
                          p->data.id, a->index + 1, line->output_count, c->cfg.output_bin_cap);
                sem_post(&p->done);
            } else if (p && !waited) {
                ++c->total_waits;
                waited = true;
                log_event(tick(c), "OUTPUT_STALL", "Посылка #%03u: выход %u полон", p->data.id, a->index + 1);
            }
            for (uint32_t i = last; i > 0; --i) {
                if (!line->cells[i] && line->cells[i - 1]) {
                    line->cells[i] = line->cells[i - 1];
                    line->cells[i - 1] = NULL;
                    if (i == 1) sem_post(&line->entry_free);
                }
            }
        }
        pthread_mutex_unlock(&c->mutex);
    }
    return NULL;
}

static void *service_thread(void *arg) {
    worker_arg_t *a = arg;
    sort_center_t *c = a->center;
    uint32_t next_output = c->cfg.output_dispatch_interval;
    uint32_t next_manual = c->cfg.manual_service_interval;
    while (pause_ticks(c, 1)) {
        uint32_t now = tick(c);
        pthread_mutex_lock(&c->mutex);
        if (now >= next_output) {
            for (uint32_t d = 0; d < c->cfg.num_destinations; ++d) {
                conveyor_line_t *line = &c->conveyors[d];
                uint32_t count = line->output_count;
                line->output_count = 0;
                c->total_shipped += count;
                for (uint32_t i = 0; i < count; ++i) sem_post(&line->output_free);
                if (count) log_event(now, "OUTPUT_SHIPPED", "Из выхода %u отгружено %u посылок", d + 1, count);
            }
            next_output = now + c->cfg.output_dispatch_interval;
        }
        if (now >= next_manual) {
            uint32_t count = c->manual_count;
            c->manual_count = 0;
            c->total_manual_serviced += count;
            for (uint32_t i = 0; i < count; ++i) sem_post(&c->manual_free);
            if (count) log_event(now, "MANUAL_SERVICED", "Оператору передано %u посылок", count);
            next_manual = now + c->cfg.manual_service_interval;
        }
        pthread_mutex_unlock(&c->mutex);
    }
    return NULL;
}

// Главный поток выполняет join и освобождает посылки по мере завершения.
static bool join_parcel(sort_center_t *c, bool workers_joined) {
    pthread_mutex_lock(&c->mutex);
    // При остановке посылка могла выйти из ожидания до завершения устройства.
    if (c->stop && !workers_joined) {
        pthread_mutex_unlock(&c->mutex);
        return false;
    }
    parcel_job_t *p = c->finished_head;
    c->finished_head = p->next_finished;
    if (!c->finished_head) c->finished_tail = NULL;
    pthread_mutex_unlock(&c->mutex);
    pthread_join(p->thread, NULL);
    sem_destroy(&p->done);
    free(p);
    return true;
}

static void start_worker(sort_center_t *c, pthread_t *threads, worker_arg_t *args,
                         uint32_t *count, void *(*function)(void *), uint32_t index) {
    if (stopped(c)) return;
    args[*count] = (worker_arg_t){.center = c, .index = index};
    if (pthread_create(&threads[*count], NULL, function, &args[*count]) != 0)
        fail(c, "Не удалось создать поток устройства/источника");
    else ++*count;
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

int sim_run(const sim_config_t *cfg, const volatile sig_atomic_t *signal_flag) {
    sort_center_t c;
    if (center_init(&c, cfg) != 0) {
        dprintf(STDERR_FILENO, "Не удалось инициализировать центр\n");
        return 1;
    }
    // Создаваемые потоки наследуют блокировку сигналов. Обработчик будет
    // выполняться только в главном потоке, где читается signal_flag.
    sigset_t signals, old_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, &old_mask) != 0) {
        center_free(&c);
        return 1;
    }
    uint32_t max_workers = cfg->num_input_lines + cfg->num_scanners + cfg->num_sorters + cfg->num_destinations + 1;
    pthread_t *threads = calloc(max_workers, sizeof(*threads));
    worker_arg_t *args = calloc(max_workers, sizeof(*args));
    uint32_t count = 0, joined_parcels = 0;
    bool interrupted = false;
    if (!threads || !args) fail(&c, "Не удалось выделить память для потоков");
    log_event(0, "SIM_START", "Многопоточный сортировочный центр: источники, устройства и посылки");
    if (cfg->unlimited_mode || cfg->num_parcels) {
        for (uint32_t i = 0; i < cfg->num_scanners; ++i) start_worker(&c, threads, args, &count, scanner_thread, i);
        for (uint32_t i = 0; i < cfg->num_sorters; ++i) start_worker(&c, threads, args, &count, sorter_thread, i);
        for (uint32_t i = 0; i < cfg->num_destinations; ++i) start_worker(&c, threads, args, &count, conveyor_thread, i);
        start_worker(&c, threads, args, &count, service_thread, 0);
        for (uint32_t i = 0; i < cfg->num_input_lines; ++i) start_worker(&c, threads, args, &count, source_thread, i);
    }
    if (pthread_sigmask(SIG_SETMASK, &old_mask, NULL) != 0)
        fail(&c, "Не удалось восстановить маску сигналов главного потока");
    while ((cfg->unlimited_mode || joined_parcels < cfg->num_parcels) && !stopped(&c)) {
        if (*signal_flag) { interrupted = true; break; }
        if (tick(&c) >= UINT32_MAX - 1000001U) { fail(&c, "Исчерпан диапазон модельного времени"); break; }
        if (timed_wait(&c.finished) == 0) {
            if (!join_parcel(&c, false)) {
                sem_post(&c.finished);
                break;
            }
            ++joined_parcels;
        }
        else if (errno != EINTR && errno != ETIMEDOUT) { fail(&c, "Ошибка ожидания завершённых посылок"); break; }
    }
    pthread_mutex_lock(&c.mutex);
    c.stop = true;
    pthread_mutex_unlock(&c.mutex);
    // Сначала устройства и источники; после их join никто больше не использует посылки.
    for (uint32_t i = 0; i < count; ++i) pthread_join(threads[i], NULL);
    while (joined_parcels < c.total_arrived) {
        while (sem_wait(&c.finished) != 0 && errno == EINTR) {}
        join_parcel(&c, true);
        ++joined_parcels;
    }
    uint64_t output_count = c.total_shipped;
    for (uint32_t d = 0; d < cfg->num_destinations; ++d) output_count += c.conveyors[d].output_count;
    if (output_count != c.total_delivered ||
        (uint64_t)c.manual_count + c.total_manual_serviced != c.total_manual_rejected ||
        (!interrupted && !c.failed && c.total_delivered + c.total_manual_rejected != c.total_arrived))
        fail(&c, "Нарушен баланс посылок");
    log_event(tick(&c), interrupted ? "SIM_INTERRUPT" : c.failed ? "SIM_ABORT" : "SIM_END",
              "Все созданные потоки завершены через pthread_join");
    print_summary(&c, STDOUT_FILENO, interrupted);
    print_summary(&c, logger_get_log_fd(), interrupted);
    int result = c.failed ? 1 : 0;
    free(args);
    free(threads);
    center_free(&c);
    return result;
}
