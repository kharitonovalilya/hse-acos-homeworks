#include "railway.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void stop_handler(int number) {
    (void)number;
    stop_requested = 1;
}

int main(int argc, char **argv) {
    Config config;
    if (read_config(argc, argv, &config) == FAIL) {
        fprintf(stderr, "Неверные параметры. Запустите с --help.\n");
        return RUN_ERROR;
    }
    if (signal(SIGINT, stop_handler) == SIG_ERR) {
        perror("Не удалось настроить Ctrl+C");
        return RUN_ERROR;
    }
    if (config.log_path != NULL) {
        log_fd = open(config.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd < 0) {
            perror("Не удалось открыть лог");
            return RUN_ERROR;
        }
    }
    RunResult result = simulate(&config);
    if (log_fd >= 0 && close(log_fd) != 0) {
        perror("Не удалось закрыть лог");
        return RUN_ERROR;
    }
    return result;
}
