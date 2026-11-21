#include "net.h"
#include "railway.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef TRAIN_CLIENT
#define ARG_COUNT 4
#define ARGUMENTS "IP PORT ID"
#else
#define ARG_COUNT 3
#define ARGUMENTS "IP PORT"
#endif

int main(int argc, char **argv) {
    struct sockaddr_in address;
    if (argc != ARG_COUNT || make_address(&address, argv[1], argv[2]) < 0) {
        dprintf(STDERR_FILENO, "Использование: %s " ARGUMENTS "\n", argv[0]);
        return RUN_ERROR;
    }

    char command[LINE_SIZE] = "WATCH\n";
#ifdef TRAIN_CLIENT
    int id;
    if (read_number(argv[3], &id) == FAIL || id < 1 || id > MAX_TRAINS) {
        dprintf(STDERR_FILENO, "Некорректный номер поезда\n");
        return RUN_ERROR;
    }
    snprintf(command, sizeof(command), "TRAIN %d\n", id);
#endif

    setup_signals();
    int fd = connect_tcp(&address);
    if (fd < 0) {
        perror("connect");
        return stop_requested ? RUN_INTERRUPTED : RUN_ERROR;
    }

    int result = RUN_ERROR;
    Line line = {0};
    if (write_all(fd, command, strlen(command)) == 0) {
        while (!stop_requested && read_line(fd, &line) == 1) {
            if (strcmp(line.data, "STOP") == 0) {
                dprintf(STDOUT_FILENO, "Сервер завершил работу\n");
                result = RUN_OK;
                break;
            }
#ifdef TRAIN_CLIENT
            if (strcmp(line.data, "GO") == 0) {
                if (write_all(fd, "ENTER\n", 6) < 0) {
                    shutdown(fd, SHUT_WR);
                }
                continue;
            }
            if (strcmp(line.data, "DONE") == 0) {
                dprintf(STDOUT_FILENO, "Поезд уже прибыл на другую станцию\n");
                continue;
            }
#endif
            if (dprintf(STDOUT_FILENO, "%s\n", line.data) < 0) break;
            if (strncmp(line.data, "ERROR ", 6) == 0) break;
        }
    }

    close(fd);
    if (stop_requested) return RUN_INTERRUPTED;
    if (result != RUN_OK && strncmp(line.data, "ERROR ", 6) != 0) {
        dprintf(STDERR_FILENO, "Соединение с сервером прервано\n");
    }
    return result;
}
