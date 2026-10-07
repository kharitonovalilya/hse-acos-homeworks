#include "railway.h"

#include <stdio.h>

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

int main(void) {
    test_numbers();
    test_config();
    test_dispatcher();
    test_state();
    test_simulation();

    if (failed_tests == 0) {
        printf("All tests passed.\n");
        return 0;
    }

    printf("Failed tests: %d\n", failed_tests);
    return 1;
}
