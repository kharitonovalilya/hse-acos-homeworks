#include "railway.h"
#include <stdlib.h>
const char *direction_name(int direction) {
    if (direction == LEFT) {
        return "L-R";
    }
    if (direction == RIGHT) {
        return "R-L";
    }
    return "NONE";
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

