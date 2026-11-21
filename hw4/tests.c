#define main server_main
#include "server.c"
#undef main

#include <assert.h>
#include <stdlib.h>

static void reset(int left, int right) {
    memset(&railway, 0, sizeof(railway));
    set_default_config(&railway.config);
    railway.config.left = left;
    railway.config.right = right;
    railway.config.arrival_max = 0;
    railway.total = left + right;
    railway.direction = EMPTY;
    railway.pending = railway.last_entry = -1;
    railway.log_fd = open("/dev/null", O_WRONLY);
    assert(railway.log_fd >= 0);
    for (int i = 0; i < MAX_PEERS; i++) railway.peers[i].fd = -1;
    for (int i = 0; i < MAX_TRAINS; i++) railway.clients[i] = -1;
    for (int i = 0; i < railway.total; i++) railway.clients[i] = i;
    make_trains(railway.trains, &railway.config);
}

static void test_priority(void) {
    reset(2, 1);
    update_trains(0);
    railway.trains[0].passenger = 0;
    railway.trains[1].passenger = 1;
    railway.trains[2].passenger = 0;
    assert(choose_train(railway.trains, 3, LEFT, 0, &railway.config) == 1);
    assert(choose_train(railway.trains, 3, LEFT, 12, &railway.config) == 0);
    railway.config.priority = 0;
    assert(choose_train(railway.trains, 3, LEFT, 0, &railway.config) == 0);
    close(railway.log_fd);
}

static void test_reconnect(void) {
    reset(1, 1);
    railway.config.priority = 0;
    enter(0, 0);
    assert(railway.occupied == 0);
    update_trains(0);
    dispatch(0);
    assert(railway.pending == 0);
    railway.clients[0] = -1;
    dispatch(1);
    assert(railway.trains[0].state == WAITING && railway.pending == 1);
    enter(1, 1);
    enter(1, 2);
    assert(railway.occupied == 1 && railway.admitted == 1);
    railway.clients[1] = -1;
    railway.clients[0] = 0;
    dispatch(3);
    assert(railway.pending < 0 && railway.direction == RIGHT);
    update_trains(6);
    dispatch(6);
    assert(railway.trains[1].state == FINISHED && railway.pending == 0);
    enter(0, 6);
    update_trains(11);
    assert(railway.finished == 2);
    close(railway.log_fd);
}

static void test_model(int left, int right, int capacity, int group, int priority) {
    reset(left, right);
    railway.config.capacity = capacity;
    railway.config.max_group = group;
    railway.config.priority = priority;
    int previous_entry = -railway.config.gap;
    int busiest = 0;
    for (int minute = 0; minute < 2000 && railway.finished < railway.total; minute++) {
        update_trains(minute);
        dispatch(minute);
        if (railway.pending >= 0) {
            assert(minute - previous_entry >= railway.config.gap);
            enter(railway.pending, minute);
            previous_entry = minute;
        }
        assert(railway.result == RUN_OK);
        assert(railway.group_size <= group);
        assert(valid_state(railway.trains, railway.total, railway.occupied,
                           railway.direction, capacity, minute, railway.config.travel));
        if (railway.occupied > busiest) busiest = railway.occupied;
    }
    assert(railway.finished == railway.total && railway.admitted == railway.total);
    if (capacity == 2 && left > 2 && group > 1) assert(busiest == 2);
    printf("OK: left=%d right=%d completed=%d max_occupancy=%d groups=%d\n",
           left, right, railway.finished, busiest, railway.groups);
    close(railway.log_fd);
}

static void test_lines(void) {
    int fd[2];
    Line line = {0};
    assert(pipe(fd) == 0 && nonblocking(fd[0]) == 0);
    assert(write_all(fd[1], "TRA", 3) == 0);
    assert(read_line(fd[0], &line) == 0 && line.length == 3);
    assert(write_all(fd[1], "IN 1\nENTER\n", 11) == 0);
    assert(read_line(fd[0], &line) == 1 && strcmp(line.data, "TRAIN 1") == 0);
    assert(read_line(fd[0], &line) == 1 && strcmp(line.data, "ENTER") == 0);
    close(fd[1]);
    assert(read_line(fd[0], &line) == -1);
    close(fd[0]);
}

int main(void) {
    test_lines();
    test_priority();
    test_reconnect();
    test_model(0, 0, 1, 1, 0);
    test_model(0, 8, 1, 1, 0);
    test_model(5, 4, 1, 1, 0);
    test_model(7, 7, 2, 3, 1);
    test_model(100, 100, 2, 3, 1);
    puts("OK: TCP line parsing, FIFO, passenger priority, waiting threshold, reconnect");
    return EXIT_SUCCESS;
}
