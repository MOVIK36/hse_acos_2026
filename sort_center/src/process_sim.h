#ifndef PROCESS_SIM_H
#define PROCESS_SIM_H
#include "config.h"
int sim_run(const sim_config_t *cfg);
int run_worker(const char *session, const char *role, const char *index);
void install_signals(void);
#endif
