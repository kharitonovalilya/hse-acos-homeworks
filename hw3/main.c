#define _POSIX_C_SOURCE 200809L
#include "model.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void stop_handler(int number) {
    (void)number;
    stop_requested = 1;
}

int main(int argc, char **argv) {
    if (signal(SIGINT, stop_handler) == SIG_ERR ||
        signal(SIGTERM, stop_handler) == SIG_ERR) {
        perror("Не удалось настроить сигналы");
        return RUN_ERROR;
    }
    int independent = 0;
    const char *session = NULL;
#ifdef INDEPENDENT
    independent = 1;
    if (argc == 4 && strcmp(argv[1], "--train") == 0) {
        int id;
        if (read_number(argv[3], &id) == FAIL) return RUN_ERROR;
        return run_train(argv[2], id);
    }
    if (argc < 3 || strcmp(argv[1], "--server") != 0) {
        fprintf(stderr, "Запуск: %s --server SESSION [параметры]\n"
                        "        %s --train SESSION ID\n", argv[0], argv[0]);
        return RUN_ERROR;
    }
    session = argv[2];
    argv[2] = argv[0];
    argc -= 2;
    argv += 2;
#endif
    Config config;
    if (read_config(argc, argv, &config) == FAIL) {
        fprintf(stderr, "Неверные параметры. Запустите с --help.\n");
        return RUN_ERROR;
    }
    char path[1024], directory[1024];
    if (config.log_path != NULL) {
        if (config.log_path[0] == '/') {
            if (strlen(config.log_path) >= sizeof path) return RUN_ERROR;
            strcpy(path, config.log_path);
        } else {
            if (getcwd(directory, sizeof directory) == NULL) return RUN_ERROR;
            int length = snprintf(path, sizeof path, "%s/%s", directory, config.log_path);
            if (length < 0 || (size_t)length >= sizeof path) return RUN_ERROR;
        }
        config.log_path = path;
    }
    RunResult result = independent ? run_server(&config, session, 1) : simulate(&config);
    if (result == RUN_ERROR) {
        fprintf(stderr, "Ошибка выполнения программы.\n");
    }
    if (log_fd >= 0 && close(log_fd) != 0) {
        perror("Не удалось закрыть лог");
        return RUN_ERROR;
    }
    return result;
}
