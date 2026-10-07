#define _POSIX_C_SOURCE 200809L
#include "railway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t stop_requested = 0;
int log_fd = -1;
int log_error = 0;

const char *direction_name(int direction) {
    if (direction == LEFT) {
        return "L-R";
    }
    if (direction == RIGHT) {
        return "R-L";
    }
    return "NONE";
}

int is_main_event(const char *event_name) {
    return strcmp(event_name, "CONFIG") == 0 ||
           strcmp(event_name, "ARRIVE") == 0 ||
           strcmp(event_name, "PERMIT") == 0 ||
           strcmp(event_name, "ENTER") == 0 ||
           strcmp(event_name, "EXIT") == 0 ||
           strcmp(event_name, "WAIT_ALERT") == 0 ||
           strcmp(event_name, "INVARIANT") == 0 ||
           strcmp(event_name, "SUMMARY") == 0;
}

void event(int minute, const char *event_name, int id, int direction,
           int occupied, const char *message) {
    if (is_main_event(event_name)) {
        if (dprintf(STDOUT_FILENO, "time=%05d event=%-9s train=%03d dir=%-4s occupancy=%d | %s\n",
                    minute, event_name, id, direction_name(direction), occupied, message) < 0)
            log_error = 1;
    }
    if (log_fd >= 0 &&
        dprintf(log_fd, "time=%05d event=%-9s train=%03d dir=%-4s occupancy=%d | %s\n",
                minute, event_name, id, direction_name(direction), occupied, message) < 0)
        log_error = 1;
}

void make_trains(Train trains[], const Config *config) {
    int total = config->left + config->right;
    srand((unsigned)config->seed);
    for (int i = 0; i < total; i++) {
        trains[i].id = i + 1;
        trains[i].direction = i < config->left ? LEFT : RIGHT;
        trains[i].passenger = rand() % 100 < config->passenger_percent;
        trains[i].arrival = config->arrival_min +
            rand() % (config->arrival_max - config->arrival_min + 1);
        trains[i].entry_time = -1;
        trains[i].exit_time = -1;
        trains[i].state = PLANNED;
    }
}

int earlier(const Train *a, const Train *b) {
    return a->arrival < b->arrival ||
           (a->arrival == b->arrival && a->id < b->id);
}

int oldest(Train trains[], int total, int direction) {
    int best = -1;
    for (int i = 0; i < total; i++) {
        if (trains[i].state != WAITING || trains[i].direction != direction) {
            continue;
        }
        if (best == -1 || earlier(&trains[i], &trains[best])) {
            best = i;
        }
    }
    return best;
}

int choose_train(Train trains[], int total, int direction,
                        int minute, const Config *config) {
    int first = oldest(trains, total, direction);
    if (first == -1 || !config->priority ||
        minute - trains[first].arrival >= config->max_wait) {
        return first;
    }
    int passenger = -1;
    for (int i = 0; i < total; i++) {
        if (trains[i].state == WAITING && trains[i].direction == direction &&
            trains[i].passenger &&
            (passenger == -1 || earlier(&trains[i], &trains[passenger])))
            passenger = i;
    }
    if (passenger == -1) {
        return first;
    }
    return passenger;
}

Direction choose_direction(Train trains[], int total, int minute,
                            const Config *config) {
    int left = choose_train(trains, total, LEFT, minute, config);
    int right = choose_train(trains, total, RIGHT, minute, config);
    if (left == -1 && right == -1) {
        return EMPTY;
    }
    if (left == -1) {
        return RIGHT;
    }
    if (right == -1) {
        return LEFT;
    }

    int oldest_left = oldest(trains, total, LEFT);
    int oldest_right = oldest(trains, total, RIGHT);
    if (!config->priority ||
        minute - trains[oldest_left].arrival >= config->max_wait ||
        minute - trains[oldest_right].arrival >= config->max_wait)
        return earlier(&trains[oldest_left], &trains[oldest_right]) ? LEFT : RIGHT;
    if (trains[left].passenger != trains[right].passenger) {
        return trains[left].passenger ? LEFT : RIGHT;
    }
    return earlier(&trains[left], &trains[right]) ? LEFT : RIGHT;
}

int valid_state(Train trains[], int total, int occupied,
                       int direction, int capacity, int minute, int travel) {
    int counted = 0;
    for (int i = 0; i < total; i++) {
        if (trains[i].state == ON_TRACK) {
            counted++;
            if (trains[i].direction != direction || trains[i].entry_time < trains[i].arrival ||
                minute - trains[i].entry_time >= travel) {
                return 0;
            }
        }
        if (trains[i].state == FINISHED &&
            trains[i].exit_time - trains[i].entry_time != travel) {
            return 0;
        }
    }
    return counted == occupied && counted <= capacity &&
           (occupied == 0 ? direction == EMPTY : direction != EMPTY);
}

void delay(int milliseconds) {
    struct timespec time = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    if (milliseconds > 0) nanosleep(&time, NULL);
}

RunResult simulate(const Config *config) {
    Train trains[MAX_TRAINS];
    int total = config->left + config->right;
    int finished = 0, occupied = 0, train_group_size = 0;
    Direction direction = EMPTY;
    int last_entry = -1, groups = 0, maximum_wait = 0, warnings = 0;
    long total_wait = 0;
    int minute = 0;
    RunResult result = RUN_OK;
    char message[1024];
    make_trains(trains, config);

    snprintf(message, sizeof message,
             "Старт: поездов %d; вместимость %d; интервал %d; группа %d; стратегия %s; seed %d",
             total, config->capacity, config->gap, config->max_group,
             config->priority ? "priority" : "fifo", config->seed);
    event(0, "CONFIG", 0, EMPTY, 0, message);

    while (1) {
        if (stop_requested) {
            result = RUN_INTERRUPTED;
            break;
        }
        if (config->until > 0 && minute >= config->until) {
            result = RUN_TIME_LIMIT;
            break;
        }

        // trains which finished leave first
        for (int i = 0; i < total; i++) {
            if (trains[i].state != ON_TRACK) {
                continue;
            }
            int elapsed = minute - trains[i].entry_time;
            if (elapsed == config->travel) {
                trains[i].state = FINISHED;
                trains[i].exit_time = minute;
                occupied--;
                finished++;
                event(minute, "EXIT", trains[i].id, trains[i].direction, occupied,
                      "Поезд покинул участок и прибыл на другую станцию");
            } else if (elapsed == (config->travel + 1) / 2) {
                event(minute, "PROGRESS", trains[i].id, trains[i].direction, occupied,
                      "Поезд проходит однопутный участок");
            }
        }
        if (direction != EMPTY && occupied == 0) {
            snprintf(message, sizeof message, "Участок свободен; группа из %d поездов завершена", train_group_size);
            event(minute, "CLEAR", 0, direction, occupied, message);
            direction = EMPTY;
            train_group_size = 0;
        }

        // add trains arriving at this minute to the queue
        for (int i = 0; i < total; i++) {
            if (trains[i].state == PLANNED && trains[i].arrival == minute) {
                trains[i].state = WAITING;
                event(minute, "ARRIVE", trains[i].id, trains[i].direction, occupied,
                      trains[i].passenger ? "Пассажирский поезд прибыл на станцию" :
                                            "Грузовой поезд прибыл на станцию");
                event(minute, "QUEUE", trains[i].id, trains[i].direction, occupied,
                      "Поезд ожидает разрешения диспетчера");
            }
        }

        // the dispatcher may let one train enter on this step
        if (occupied < config->capacity &&
            (last_entry == -1 || minute - last_entry >= config->gap)) {
            if (direction == EMPTY) {
                direction = choose_direction(trains, total, minute, config);
                if (direction != EMPTY) {
                    groups++;
                    event(minute, "DIRECTION", 0, direction, occupied,
                          "Диспетчер установил направление движения");
                }
            }
            if (direction != EMPTY && train_group_size < config->max_group) {
                int index = choose_train(trains, total, direction, minute, config);
                if (index != -1) {
                    Train *train = &trains[index];
                    int waited = minute - train->arrival;
                    if (waited >= config->max_wait) {
                        warnings++;
                        event(minute, "WAIT_ALERT", train->id, direction, occupied,
                              "Поезд достиг порога ожидания; безопасность важнее срока");
                    }
                    snprintf(message, sizeof message, "Диспетчер разрешил въезд; ожидание %d мин", waited);
                    event(minute, "PERMIT", train->id, direction, occupied, message);
                    train->state = ON_TRACK;
                    train->entry_time = minute;
                    occupied++;
                    train_group_size++;
                    last_entry = minute;
                    total_wait += waited;
                    if (waited > maximum_wait) {
                        maximum_wait = waited;
                    }
                    event(minute, "ENTER", train->id, direction, occupied,
                          "Поезд въехал на участок");
                    if (config->travel == 1) {
                        event(minute, "PROGRESS", train->id, direction, occupied,
                              "Поезд проходит однопутный участок");
                    }
                }
            }
        }

        // check the main safety rules after every step.
        if (!valid_state(trains, total, occupied, direction,
                         config->capacity, minute, config->travel)) {
            event(minute, "INVARIANT", 0, direction, occupied,
                  "Ошибка: нарушено правило движения");
            result = RUN_INVARIANT_ERROR;
            break;
        }
        if (finished == total) {
            break;
        }
        delay(config->delay_ms);
        minute++;
    }

    int admitted = finished + occupied;
    const char *reason;
    if (result == RUN_OK) {
        reason = "все поезда завершили путь";
    } else if (result == RUN_TIME_LIMIT) {
        reason = "достигнут предел модельного времени";
    } else if (result == RUN_INTERRUPTED) {
        reason = "прервано пользователем";
    } else {
        reason = "ошибка инварианта";
    }

    snprintf(message, sizeof message,
             "Итог: %s; завершено %d/%d; осталось %d; групп %d; среднее ожидание %.2f мин; максимум %d мин; достигли порога %d",
             reason, finished, total, total - finished, groups,
             admitted ? (double)total_wait / admitted : 0.0, maximum_wait, warnings);
    event(minute, "SUMMARY", 0, direction, occupied, message);
    if (config->log_path != NULL) {
        if (dprintf(STDOUT_FILENO, "Подробный лог записан в файл: %s\n",
                    config->log_path) < 0)
            log_error = 1;
    }
    if (log_error) {
        return RUN_ERROR;
    }
    return result;
}
