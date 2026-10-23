#define _POSIX_C_SOURCE 200809L
#include "railway.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int failed_tests = 0;

void check(int condition, const char *name) {
    if (condition) {
        printf("PASSED: %s\n", name);
    } else {
        printf("FAILED: %s\n", name);
        failed_tests++;
    }
}

void test_numbers(void) {
    int number;

    check(read_number("123", &number) == SUCCESS && number == 123,
          "correct number");
    check(read_number("12x", &number) == FAIL,
          "letters in number");
    check(read_number("", &number) == FAIL,
          "empty number");
    check(read_number("1000001", &number) == FAIL,
          "number is too large");
}

void test_config(void) {
    Config config;
    char *correct[] = {"railway", "--left", "3", "--capacity", "1"};
    char *incorrect[] = {"railway", "--capacity", "0"};
    char *without_value[] = {"railway", "--left"};

    check(read_config(5, correct, &config) == SUCCESS &&
          config.left == 3 && config.capacity == 1,
          "correct arguments");
    check(read_config(3, incorrect, &config) == FAIL,
          "incorrect capacity");
    check(read_config(2, without_value, &config) == FAIL,
          "argument without value");
}

void test_dispatcher(void) {
    Config config;
    set_default_config(&config);

    Train trains[] = {
        {1, LEFT, 0, 0, -1, -1, WAITING},
        {2, LEFT, 1, 2, -1, -1, WAITING},
        {3, RIGHT, 1, 1, -1, -1, WAITING}
    };

    config.priority = 0;
    check(choose_direction(trains, 3, 3, &config) == LEFT,
          "FIFO chooses the oldest train");

    config.priority = 1;
    check(choose_direction(trains, 3, 3, &config) == RIGHT,
          "passenger train has priority");
    check(choose_direction(trains, 3, 12, &config) == LEFT,
          "old train does not wait forever");
}

void test_state(void) {
    Train trains[] = {
        {1, LEFT, 1, 0, 1, -1, ON_TRACK},
        {2, LEFT, 0, 0, 2, -1, ON_TRACK}
    };

    check(valid_state(trains, 2, 2, LEFT, 2, 3, 5),
          "correct track state");
    check(!valid_state(trains, 2, 2, LEFT, 1, 3, 5),
          "capacity cannot be exceeded");

    trains[1].direction = RIGHT;
    check(!valid_state(trains, 2, 2, LEFT, 2, 3, 5),
          "opposite directions are forbidden");
}

void test_simulation(void) {
    Config config;
    set_default_config(&config);
    config.left = 2;
    config.right = 2;
    config.arrival_min = 0;
    config.arrival_max = 2;
    config.travel = 3;
    config.gap = 1;
    config.log_path = NULL;
    config.delay_ms = 1;

    config.capacity = 1;
    check(simulate(&config) == RUN_OK,
          "simple mode");

    config.capacity = 2;
    check(simulate(&config) == RUN_OK,
          "group mode");

    config.until = 1;
    check(simulate(&config) == RUN_TIME_LIMIT,
          "time limit");

    config.until = 0;
    stop_requested = 1;
    check(simulate(&config) == RUN_INTERRUPTED,
          "interrupted simulation");
    stop_requested = 0;
}

int check_log(FILE *file, const Config *config, int total) {
    int permitted[MAX_TRAINS] = {0}, entered[MAX_TRAINS] = {0};
    int exited[MAX_TRAINS] = {0}, entry_time[MAX_TRAINS] = {0};
    int active = 0, direction = EMPTY, last_entry = -1, group_size = 0;
    int finished = 0;
    char line[2048], name[32], dir[16];
    int minute, id, occupied;

    while (fgets(line, sizeof line, file) != NULL) {
        if (sscanf(line, "time=%d event=%31s train=%d dir=%15s occupancy=%d",
                   &minute, name, &id, dir, &occupied) != 5) return 0;
        if (strcmp(name, "INVARIANT") == 0) return 0;
        if (strcmp(name, "DIRECTION") == 0) {
            if (active != 0) return 0;
            group_size = 0;
        }
        if (id == 0) continue;
        if (id < 1 || id > total) return 0;
        int index = id - 1;
        int train_direction = strcmp(dir, "L-R") == 0 ? LEFT : RIGHT;
        if (strcmp(name, "PERMIT") == 0) {
            if (permitted[index]++) return 0;
        } else if (strcmp(name, "ENTER") == 0) {
            if (!permitted[index] || entered[index]++) return 0;
            if (active > 0 && direction != train_direction) return 0;
            if (last_entry >= 0 && minute - last_entry < config->gap) return 0;
            direction = train_direction;
            last_entry = minute;
            entry_time[index] = minute;
            active++;
            group_size++;
            if (active > config->capacity || active != occupied ||
                group_size > config->max_group) return 0;
        } else if (strcmp(name, "EXIT") == 0) {
            if (!entered[index] || exited[index]++) return 0;
            if (minute - entry_time[index] < config->travel) return 0;
            active--;
            finished++;
            if (active < 0 || active != occupied) return 0;
        }
    }
    return !ferror(file) && active == 0 && finished == total;
}

void test_full_log(int capacity, int left, int right, int priority) {
    Config config;
    set_default_config(&config);
    config.left = left;
    config.right = right;
    config.capacity = capacity;
    config.max_group = 3;
    config.arrival_max = 3;
    config.travel = 8;
    config.gap = 1;
    config.delay_ms = 2;
    config.priority = priority;
    config.log_path = NULL;
    char path[] = "/tmp/railway-test-XXXXXX";
    log_fd = mkstemp(path);
    if (log_fd < 0) {
        check(0, "create test log");
        return;
    }
    RunResult result = simulate(&config);
    lseek(log_fd, 0, SEEK_SET);
    FILE *file = fdopen(log_fd, "r");
    int correct = file != NULL && check_log(file, &config, left + right);
    if (file != NULL) fclose(file);
    else close(log_fd);
    log_fd = -1;
    unlink(path);
    check(result == RUN_OK && correct, "full log follows all movement rules");
}

int main(void) {
    test_numbers();
    test_config();
    test_dispatcher();
    test_state();
    test_simulation();
    test_full_log(1, 3, 3, 0);
    test_full_log(3, 5, 4, 1);
    test_full_log(2, 0, 4, 1);
    test_full_log(2, 4, 0, 0);
    test_full_log(2, 0, 0, 0);

    if (failed_tests == 0) {
        printf("All tests passed.\n");
        return 0;
    }

    printf("Failed tests: %d\n", failed_tests);
    return 1;
}
