#ifndef MODEL_H
#define MODEL_H

#include "railway.h"
#include "sync.h"
#include <time.h>

typedef struct {
    // shared model data is protected by mutex
    Config config;
    char log_path[1024];
    int ready;
    int workers[MAX_TRAINS];
    Train trains[MAX_TRAINS];
    Notification mutex;
    Notification changed;
    Notification permits[MAX_TRAINS];
    struct timespec start;
    int total, occupied, finished, stopped, pending;
    int group_size, groups, last_entry, maximum_wait, admitted, warnings;
    long total_wait;
    Direction direction;
    RunResult result;
} Railway;

void model_lock(Railway *railway);
void model_unlock(Railway *railway);
int current_minute(Railway *railway);
void make_trains(Train trains[], const Config *config);
void event(int minute, const char *name, int id, int direction, int occupied,
           const char *message);
void stop_all(Railway *railway, RunResult result);
void train_process(Railway *railway, int index);
void dispatcher_process(Railway *railway);
RunResult run_server(const Config *config, const char *session, int independent);
RunResult run_train(const char *session, int id);

#endif
