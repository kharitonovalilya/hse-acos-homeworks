#define _POSIX_C_SOURCE 200809L
#include "railway.h"
#include "sync.h"

#include <pthread.h>

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

void event(int minute, const char *event_name, int id, int direction,
           int occupied, const char *message) {
    if (dprintf(STDOUT_FILENO, "time=%05d event=%-9s train=%03d dir=%-4s occupancy=%d | %s\n",
                minute, event_name, id, direction_name(direction), occupied, message) < 0)
        log_error = 1;
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

typedef struct {
    // shared model data is protected by mutex
    Config config;
    Train trains[MAX_TRAINS];
    pthread_mutex_t mutex;
    Notification changed;
    Notification permits[MAX_TRAINS];
    struct timespec start;
    int total, occupied, finished, stopped, pending;
    int group_size, groups, last_entry, maximum_wait, admitted, warnings;
    long total_wait;
    Direction direction;
    RunResult result;
} Railway;

typedef struct {
    Railway *railway;
    int index;
} TrainArgument;

int current_minute(Railway *railway) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long milliseconds = (now.tv_sec - railway->start.tv_sec) * 1000L +
        (now.tv_nsec - railway->start.tv_nsec) / 1000000L;
    int step = railway->config.delay_ms;
    if (step == 0) step = 1;
    return (int)(milliseconds / step);
}

void stop_all(Railway *railway, RunResult result) {
    railway->stopped = 1;
    railway->result = result;
    for (int i = 0; i < railway->total; i++) {
        notify_post(&railway->permits[i]);
    }
    notify_post(&railway->changed);
}

// the caller holds the mutex here and during every state change
int wait_time(Railway *railway, int index, int minute) {
    while (!railway->stopped && current_minute(railway) < minute) {
        pthread_mutex_unlock(&railway->mutex);
        int result = wait_event(&railway->permits[index]);
        pthread_mutex_lock(&railway->mutex);
        if (result < 0) stop_all(railway, RUN_ERROR);
    }
    return !railway->stopped;
}

int valid_state(Train trains[], int total, int occupied,
                int direction, int capacity, int minute, int travel) {
    int counted = 0;
    (void)minute;
    for (int i = 0; i < total; i++) {
        if (trains[i].state == ON_TRACK) {
            counted++;
            if (trains[i].direction != direction ||
                trains[i].entry_time < trains[i].arrival) return 0;
        }
        if (trains[i].state == FINISHED &&
            trains[i].exit_time - trains[i].entry_time < travel) return 0;
    }
    return counted == occupied && counted <= capacity &&
           (occupied == 0 || direction != EMPTY);
}

void check_state(Railway *railway) {
    if (!valid_state(railway->trains, railway->total, railway->occupied,
                     railway->direction, railway->config.capacity,
                     current_minute(railway), railway->config.travel)) {
        event(current_minute(railway), "INVARIANT", 0, railway->direction,
              railway->occupied, "Ошибка: нарушено правило движения");
        stop_all(railway, RUN_INVARIANT_ERROR);
    }
}

void *train_thread(void *argument) {
    TrainArgument *data = argument;
    Railway *railway = data->railway;
    int index = data->index;
    Train *train = &railway->trains[index];
    pthread_mutex_lock(&railway->mutex);

    if (!wait_time(railway, index, train->arrival)) {
        pthread_mutex_unlock(&railway->mutex);
        return NULL;
    }
    train->state = WAITING;
    event(current_minute(railway), "ARRIVE", train->id, train->direction,
          railway->occupied, train->passenger ? "Пассажирский поезд прибыл на станцию" :
                                               "Грузовой поезд прибыл на станцию");
    event(current_minute(railway), "QUEUE", train->id, train->direction,
          railway->occupied, "Поезд ожидает разрешения диспетчера");
    notify_post(&railway->changed);

    // only this train receives its own entry permit
    while (!railway->stopped && train->state != PERMITTED) {
        pthread_mutex_unlock(&railway->mutex);
        int result = wait_event(&railway->permits[index]);
        pthread_mutex_lock(&railway->mutex);
        if (result < 0) stop_all(railway, RUN_ERROR);
    }
    if (railway->stopped) {
        pthread_mutex_unlock(&railway->mutex);
        return NULL;
    }

    int minute = current_minute(railway);
    int waited = minute - train->arrival;
    if (train->state != PERMITTED || railway->occupied >= railway->config.capacity ||
        train->direction != railway->direction ||
        (railway->last_entry >= 0 && minute - railway->last_entry < railway->config.gap)) {
        event(minute, "INVARIANT", train->id, train->direction,
              railway->occupied, "Ошибка разрешения на въезд");
        stop_all(railway, RUN_INVARIANT_ERROR);
        pthread_mutex_unlock(&railway->mutex);
        return NULL;
    }
    train->state = ON_TRACK;
    train->entry_time = minute;
    railway->pending = 0;
    railway->occupied++;
    railway->group_size++;
    railway->last_entry = minute;
    railway->admitted++;
    railway->total_wait += waited;
    if (waited > railway->maximum_wait) railway->maximum_wait = waited;
    if (waited >= railway->config.max_wait) {
        railway->warnings++;
        event(minute, "WAIT_ALERT", train->id, train->direction,
              railway->occupied, "Поезд достиг порога ожидания");
    }
    event(minute, "ENTER", train->id, train->direction,
          railway->occupied, "Поезд въехал на участок");
    check_state(railway);
    notify_post(&railway->changed);

    if (wait_time(railway, index, minute + railway->config.travel / 2)) {
        event(current_minute(railway), "PROGRESS", train->id, train->direction,
              railway->occupied, "Поезд проходит однопутный участок");
    }
    if (wait_time(railway, index, minute + railway->config.travel)) {
        train->state = FINISHED;
        train->exit_time = current_minute(railway);
        railway->occupied--;
        railway->finished++;
        event(train->exit_time, "EXIT", train->id, train->direction,
              railway->occupied, "Поезд покинул участок и прибыл на другую станцию");
        check_state(railway);
        notify_post(&railway->changed);
    }
    pthread_mutex_unlock(&railway->mutex);
    return NULL;
}

void *dispatcher_thread(void *argument) {
    Railway *railway = argument;
    pthread_mutex_lock(&railway->mutex);
    while (!railway->stopped && railway->finished < railway->total) {
        int minute = current_minute(railway);
        if (stop_requested) {
            stop_all(railway, RUN_INTERRUPTED);
            break;
        }
        if (railway->config.until > 0 && minute >= railway->config.until) {
            stop_all(railway, RUN_TIME_LIMIT);
            break;
        }
        if (log_error) {
            stop_all(railway, RUN_ERROR);
            break;
        }
        if (railway->occupied == 0 && !railway->pending && railway->direction != EMPTY) {
            event(minute, "CLEAR", 0, railway->direction, 0, "Участок освобождён");
            railway->direction = EMPTY;
            railway->group_size = 0;
        }
        if (!railway->pending && railway->occupied < railway->config.capacity &&
            (railway->last_entry < 0 || minute - railway->last_entry >= railway->config.gap)) {
            if (railway->direction == EMPTY) {
                railway->direction = choose_direction(railway->trains, railway->total,
                                                      minute, &railway->config);
                if (railway->direction != EMPTY) {
                    railway->groups++;
                    event(minute, "DIRECTION", 0, railway->direction, railway->occupied,
                          "Диспетчер установил направление движения");
                }
            }
            if (railway->direction != EMPTY && railway->group_size < railway->config.max_group) {
                int index = choose_train(railway->trains, railway->total,
                                         railway->direction, minute, &railway->config);
                if (index >= 0) {
                    railway->trains[index].state = PERMITTED;
                    railway->pending = 1;
                    event(minute, "PERMIT", railway->trains[index].id, railway->direction,
                          railway->occupied, "Диспетчер разрешил въезд");
                    notify_post(&railway->permits[index]);
                }
            }
        }
        pthread_mutex_unlock(&railway->mutex);
        int result = wait_event(&railway->changed);
        pthread_mutex_lock(&railway->mutex);
        if (result < 0) stop_all(railway, RUN_ERROR);
    }
    if (!railway->stopped && stop_requested) stop_all(railway, RUN_INTERRUPTED);
    if (railway->occupied == 0 && railway->direction != EMPTY) {
        event(current_minute(railway), "CLEAR", 0, railway->direction, 0, "Участок освобождён");
        railway->direction = EMPTY;
    }
    pthread_mutex_unlock(&railway->mutex);
    return NULL;
}

RunResult simulate(const Config *config) {
    Railway railway = {0};
    pthread_t trains[MAX_TRAINS], dispatcher;
    TrainArgument arguments[MAX_TRAINS];
    railway.config = *config;
    railway.total = config->left + config->right;
    railway.direction = EMPTY;
    railway.last_entry = -1;
    log_error = 0;
    make_trains(railway.trains, config);
    if (pthread_mutex_init(&railway.mutex, NULL) != 0) return RUN_ERROR;
    if (notify_init(&railway.changed) != 0) {
        pthread_mutex_destroy(&railway.mutex);
        return RUN_ERROR;
    }
    int initialized = 0;
    for (; initialized < railway.total; initialized++) {
        if (notify_init(&railway.permits[initialized]) != 0) break;
    }
    if (initialized != railway.total) {
        for (int i = 0; i < initialized; i++) notify_destroy(&railway.permits[i]);
        notify_destroy(&railway.changed);
        pthread_mutex_destroy(&railway.mutex);
        return RUN_ERROR;
    }
    clock_gettime(CLOCK_MONOTONIC, &railway.start);
    char message[512];
    snprintf(message, sizeof message,
             "Старт: поездов %d; вместимость %d; интервал %d; группа %d; стратегия %s; seed %d",
             railway.total, config->capacity, config->gap, config->max_group,
             config->priority ? "priority" : "fifo", config->seed);
    event(0, "CONFIG", 0, EMPTY, 0, message);

    int created = 0;
    for (; created < railway.total; created++) {
        arguments[created].railway = &railway;
        arguments[created].index = created;
        if (pthread_create(&trains[created], NULL, train_thread, &arguments[created]) != 0) break;
    }
    int dispatcher_created = 0;
    if (created == railway.total &&
        pthread_create(&dispatcher, NULL, dispatcher_thread, &railway) == 0) {
        dispatcher_created = 1;
    } else {
        pthread_mutex_lock(&railway.mutex);
        stop_all(&railway, RUN_ERROR);
        pthread_mutex_unlock(&railway.mutex);
    }

    // all threads finish before their arguments and semaphores are removed
    if (dispatcher_created) pthread_join(dispatcher, NULL);
    for (int i = 0; i < created; i++) pthread_join(trains[i], NULL);
    const char *reason = "все поезда завершили путь";
    if (railway.result == RUN_INTERRUPTED) reason = "прервано пользователем";
    else if (railway.result == RUN_TIME_LIMIT) reason = "достигнут предел времени";
    else if (railway.result != RUN_OK) reason = "ошибка выполнения";
    snprintf(message, sizeof message,
             "Итог: %s; завершено %d/%d; осталось %d; групп %d; среднее ожидание %.2f мин; максимум %d мин; достигли порога %d",
             reason, railway.finished, railway.total, railway.total - railway.finished,
             railway.groups, railway.admitted ? (double)railway.total_wait / railway.admitted : 0.0,
             railway.maximum_wait, railway.warnings);
    event(current_minute(&railway), "SUMMARY", 0, railway.direction, railway.occupied, message);
    if (config->log_path != NULL && log_fd >= 0 && !log_error) {
        if (dprintf(STDOUT_FILENO, "Подробный лог записан в файл: %s\n", config->log_path) < 0)
            log_error = 1;
    }
    for (int i = 0; i < initialized; i++) notify_destroy(&railway.permits[i]);
    notify_destroy(&railway.changed);
    pthread_mutex_destroy(&railway.mutex);
    if (log_error) return RUN_ERROR;
    return railway.result;
}
