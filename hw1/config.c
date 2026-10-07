#include "railway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void usage(const char *program) {
    printf("Использование: %s [параметры]\n"
           "  --left N --right N  число поездов с каждой станции (всего не более 200)\n"
           "  --passenger-percent N  вероятность пассажирского поезда (0..100)\n"
           "  --arrival-min N --arrival-max N  диапазон времени прибытия\n"
           "  --travel N  время прохождения участка\n"
           "  --capacity N  1 для простого режима, больше 1 для группового\n"
           "  --gap N  минимальный интервал между въездами\n"
           "  --max-group N  максимальное число поездов в группе\n"
           "  --strategy fifo|priority  обычная очередь или приоритет пассажирских поездов\n"
           "  --max-wait N  порог ожидания для повышения приоритета\n"
           "  --seed N  начальное значение генератора случайных чисел\n"
           "  --delay-ms N  задержка между шагами в миллисекундах\n"
           "  --until N  остановка на указанной модельной минуте\n"
           "  --log PATH  имя файла лога\n", program);
}

Result read_number(const char *text, int *result) {
    if (text[0] == '\0') {
        return FAIL;
    }

    long number = 0;

    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return FAIL;
        }
        int digit = text[i] - '0';
        number = number * 10 + digit;
        if (number > 1000000) {
            return FAIL;
        }
    }

    *result = (int)number;
    return SUCCESS;
}

void set_default_config(Config *config) {
    config->left = 7;
    config->right = 7;
    config->passenger_percent = 50;
    config->arrival_min = 0;
    config->arrival_max = 12;
    config->travel = 5;
    config->capacity = 2;
    config->gap = 2;
    config->max_group = 3;
    config->max_wait = 12;
    config->priority = 1;
    config->delay_ms = 0;
    config->until = 0;
    config->seed = 42;
    config->log_path = "railway.log";
}

Result read_config(int argc, char **argv, Config *config) {
    set_default_config(config);

    for (int i = 1; i < argc; i++) {
        const char *key = argv[i];

        if (strcmp(key, "--help") == 0) { usage(argv[0]); exit(RUN_OK); }
        if (i + 1 >= argc) return FAIL;
        const char *value = argv[++i];
        if (strcmp(key, "--log") == 0) {
            config->log_path = value;
            continue;
        }
        if (strcmp(key, "--strategy") == 0) {
            if (strcmp(value, "fifo") == 0) config->priority = 0;
            else if (strcmp(value, "priority") == 0) config->priority = 1;
            else return FAIL;
            continue;
        }

        int number;
        if (read_number(value, &number) == FAIL) return FAIL;

        if (strcmp(key, "--left") == 0) config->left = number;
        else if (strcmp(key, "--right") == 0) config->right = number;
        else if (strcmp(key, "--passenger-percent") == 0) config->passenger_percent = number;
        else if (strcmp(key, "--arrival-min") == 0) config->arrival_min = number;
        else if (strcmp(key, "--arrival-max") == 0) config->arrival_max = number;
        else if (strcmp(key, "--travel") == 0) config->travel = number;
        else if (strcmp(key, "--capacity") == 0) config->capacity = number;
        else if (strcmp(key, "--gap") == 0) config->gap = number;
        else if (strcmp(key, "--max-group") == 0) config->max_group = number;
        else if (strcmp(key, "--max-wait") == 0) config->max_wait = number;
        else if (strcmp(key, "--seed") == 0) config->seed = number;
        else if (strcmp(key, "--delay-ms") == 0) config->delay_ms = number;
        else if (strcmp(key, "--until") == 0) config->until = number;
        else return FAIL;
    }

    // checking all values after reading the command line
    if (config->left + config->right > MAX_TRAINS ||
        config->passenger_percent > 100 ||
        config->arrival_min > config->arrival_max || config->arrival_max > 100000 ||
        config->travel < 1 || config->travel > 100000 ||
        config->capacity < 1 || config->capacity > MAX_TRAINS ||
        config->gap < 1 || config->gap > 100000 ||
        config->max_group < 1 || config->max_group > MAX_TRAINS ||
        config->max_wait < 1 || config->delay_ms > 1000 || config->until > 100000)
        return FAIL;

    return SUCCESS;
}
