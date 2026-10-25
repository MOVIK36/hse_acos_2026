#ifndef THREAD_SIM_H
#define THREAD_SIM_H
#include <signal.h>
#include "config.h"

int sim_run(const sim_config_t *cfg, const volatile sig_atomic_t *signal_flag);
#endif
