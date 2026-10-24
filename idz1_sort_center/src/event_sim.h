#ifndef EVENT_SIM_H
#define EVENT_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include "center.h"

// Статусы завершения симуляции
typedef enum {
    SIM_STATUS_RUNNING = 0,
    SIM_STATUS_COMPLETED = 1,
    SIM_STATUS_INTERRUPTED = 2,
    SIM_STATUS_INVARIANT_ERROR = 3,
    SIM_STATUS_RESOURCE_ERROR = 4
} sim_status_t;

typedef struct {
    sort_center_t center;
    sim_status_t status;
    bool stop_requested;
    const volatile sig_atomic_t *signal_flag;
} event_simulation_t;

int sim_init(event_simulation_t *sim, const sim_config_t *cfg);
void sim_cleanup(event_simulation_t *sim);

// Шаг моделирования (1 такт)
bool sim_step(event_simulation_t *sim);

// Основной цикл симуляции
sim_status_t sim_run(event_simulation_t *sim);

// Запрос мягкого завершения (по сигналу SIGINT)
void sim_request_stop(event_simulation_t *sim);

// Печать итоговой статистики в дескриптор
void sim_print_summary(const event_simulation_t *sim, int out_fd);

#endif /* EVENT_SIM_H */
