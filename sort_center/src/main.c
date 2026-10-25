#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "io_logger.h"
#include "thread_sim.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop_received = 0;

static void signal_handler(int signo) {
    (void)signo;
    stop_received = 1;
}

int main(int argc, char *argv[]) {
    sim_config_t cfg;
    config_set_defaults(&cfg);
    int parsed = config_parse_args(&cfg, argc, argv);
    if (parsed) return parsed > 0 ? 0 : 1;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) return 1;
    if (logger_init(cfg.log_filepath, cfg.verbose_stdout) != 0) return 1;
    config_print(&cfg, STDOUT_FILENO);
    config_print(&cfg, logger_get_log_fd());
    int result = sim_run(&cfg, &stop_received);
    logger_close();
    return result;
}
