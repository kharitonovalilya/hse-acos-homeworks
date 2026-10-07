#ifndef RAILWAY_H
#define RAILWAY_H

#include <signal.h>

#define MAX_TRAINS 200

typedef enum {
    EMPTY = -1,
    LEFT,
    RIGHT
} Direction;

typedef enum {
    PLANNED,
    WAITING,
    ON_TRACK,
    FINISHED
} TrainState;

typedef enum {
    SUCCESS,
    FAIL
} Result;

typedef enum {
    RUN_OK,
    RUN_ERROR,
    RUN_TIME_LIMIT,
    RUN_INVARIANT_ERROR,
    RUN_INTERRUPTED
} RunResult;

typedef struct {
    int id;
    Direction direction;
    int passenger;
    int arrival;
    int entry_time;
    int exit_time;
    TrainState state;
} Train;

typedef struct {
    int left, right, passenger_percent;
    int arrival_min, arrival_max, travel;
    int capacity, gap, max_group, max_wait;
    int priority, delay_ms, until, seed;
    const char *log_path;
} Config;

extern volatile sig_atomic_t stop_requested;
extern int log_fd;
extern int log_error;

Result read_number(const char *text, int *result);
void set_default_config(Config *config);
Result read_config(int argc, char **argv, Config *config);
RunResult simulate(const Config *config);

int is_main_event(const char *event_name);
int oldest(Train trains[], int total, int direction);
int choose_train(Train trains[], int total, int direction,
                 int minute, const Config *config);
Direction choose_direction(Train trains[], int total, int minute,
                           const Config *config);
int valid_state(Train trains[], int total, int occupied,
                int direction, int capacity, int minute, int travel);

#endif
