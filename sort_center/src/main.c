#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "io_logger.h"
#include "process_sim.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    install_signals();
    // Независимый участник получает настройки из общей памяти координатора.
    if (argc == 5 && strcmp(argv[1], "--worker") == 0)
        return run_worker(argv[2], argv[3], argv[4]);
    sim_config_t cfg;
    config_set_defaults(&cfg);
    int parsed = config_parse_args(&cfg, argc, argv);
    if (parsed) return parsed > 0 ? 0 : 1;
    if (!cfg.session[0]) snprintf(cfg.session, sizeof(cfg.session), "run_%ld", (long)getpid());
    // Независимые участники могут запускаться из другого рабочего каталога.
    if (cfg.log_filepath[0] != '/') {
        char path[sizeof(cfg.log_filepath)], cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) return 1;
        int length = snprintf(path, sizeof(path), "%s/%s", cwd, cfg.log_filepath);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            dprintf(STDERR_FILENO, "Слишком длинный путь к журналу\n");
            return 1;
        }
        strcpy(cfg.log_filepath, path);
    }
    int result = sim_run(&cfg);
    logger_close();
    return result;
}
