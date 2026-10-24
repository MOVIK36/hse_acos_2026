#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include "config.h"
#include "io_logger.h"
#include "event_sim.h"

static event_simulation_t g_sim;
static volatile sig_atomic_t g_sigint_received = 0;

// Обработчик Ctrl+C
static void sigint_handler(int signo) {
    (void)signo;
    g_sigint_received = 1;
}

static int setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

int main(int argc, char *argv[]) {
    sim_config_t cfg;
    config_set_defaults(&cfg);

    int parse_rc = config_parse_args(&cfg, argc, argv);
    if (parse_rc != 0) {
        return (parse_rc > 0) ? 0 : 1;
    }

    // Инициализируем логирование в файл (дескриптор log_fd)
    if (logger_init(cfg.log_filepath, cfg.verbose_stdout) != 0) {
        dprintf(STDERR_FILENO, "[ERROR] Не удалось открыть лог-файл.\n");
        return 1;
    }

    srand(cfg.random_seed);

    // Печать параметров в STDOUT и в лог
    config_print(&cfg, STDOUT_FILENO);
    int log_fd = logger_get_log_fd();
    if (log_fd >= 0) {
        config_print(&cfg, log_fd);
    }

    if (sim_init(&g_sim, &cfg) != 0) {
        dprintf(STDERR_FILENO, "[ERROR] Ошибка инициализации симуляции.\n");
        logger_close();
        return 1;
    }

    g_sim.signal_flag = &g_sigint_received;
    if (setup_signal_handlers() != 0) {
        dprintf(STDERR_FILENO, "[ERROR] Не удалось установить обработчики сигналов.\n");
        sim_cleanup(&g_sim);
        logger_close();
        return 1;
    }
    sim_run(&g_sim);

    // Печать итогового отчета
    sim_print_summary(&g_sim, STDOUT_FILENO);
    if (log_fd >= 0) {
        sim_print_summary(&g_sim, log_fd);
    }

    sim_cleanup(&g_sim);
    logger_close();

    return (g_sim.status == SIM_STATUS_INVARIANT_ERROR || g_sim.status == SIM_STATUS_RESOURCE_ERROR) ? 1 : 0;
}