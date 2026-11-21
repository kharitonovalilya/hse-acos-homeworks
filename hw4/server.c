#include "net.h"
#include "railway.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PEERS (MAX_TRAINS + 32)
#define OUTPUT_SIZE 16384
#define ACK_TIMEOUT_MS 2000

typedef enum { NEW, OBSERVER, TRAIN, CLOSING } Role;

typedef struct {
    int fd, train, stopping;
    Role role;
    Line input;
    char output[OUTPUT_SIZE];
    size_t start, end;
} Peer;

typedef struct {
    Config config;
    Train trains[MAX_TRAINS];
    int clients[MAX_TRAINS];
    Peer peers[MAX_PEERS];
    struct timespec start;
    int total, occupied, finished, pending;
    int group_size, groups, last_entry, admitted, warnings, maximum_wait;
    long total_wait;
    long long permit_deadline;
    Direction direction;
    int progressed[MAX_TRAINS];
    int log_fd, log_error;
    RunResult result;
} Railway;

static Railway railway;

static long long milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int current_minute(void) {
    long long start = (long long)railway.start.tv_sec * 1000 +
                      railway.start.tv_nsec / 1000000;
    int step = railway.config.delay_ms ? railway.config.delay_ms : 1;
    long long minute = (milliseconds() - start) / step;
    return minute > 1000000000 ? 1000000000 : (int)minute;
}

static void disconnect(int slot) {
    Peer *peer = &railway.peers[slot];
    if (peer->fd < 0) return;
    if (peer->role == TRAIN) {
        railway.clients[peer->train] = -1;
    }
    close(peer->fd);
    peer->fd = -1;
}

// queue the message so the server can keep working
static void send_line(int slot, const char *text) {
    Peer *peer = &railway.peers[slot];
    if (peer->fd < 0) return;
    size_t length = strlen(text);
    if (length > sizeof(peer->output) - peer->end && peer->start > 0) {
        memmove(peer->output, peer->output + peer->start, peer->end - peer->start);
        peer->end -= peer->start;
        peer->start = 0;
    }
    if (length > sizeof(peer->output) - peer->end) {
        disconnect(slot);
        return;
    }
    memcpy(peer->output + peer->end, text, length);
    peer->end += length;
}

static void flush(int slot) {
    Peer *peer = &railway.peers[slot];
    if (peer->fd < 0 || peer->end == peer->start) return;
    ssize_t count = write(peer->fd, peer->output + peer->start, peer->end - peer->start);
    if (count > 0) {
        peer->start += (size_t)count;
        if (peer->start == peer->end) {
            peer->start = peer->end = 0;
            if (peer->role == CLOSING && !peer->stopping) disconnect(slot);
        }
        return;
    }
    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        disconnect(slot);
    }
}

static void record(const char *line) {
    if (write_all(railway.log_fd, line, strlen(line)) < 0) railway.log_error = 1;
}

static void event(int minute, const char *name, int index, Direction direction,
                  const char *message) {
    char line[LINE_SIZE];
    snprintf(line, sizeof(line), "time=%05d event=%-10s train=%03d dir=%-4s occupancy=%d | %s\n",
             minute, name, index + 1, direction_name(direction), railway.occupied, message);
    record(line);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (railway.peers[i].fd >= 0 &&
            (railway.peers[i].role == OBSERVER ||
             (railway.peers[i].role == TRAIN && railway.peers[i].train == index))) {
            send_line(i, line);
        }
    }
}

// send the current state to each new observer
static void snapshot(int slot, int minute) {
    static const char *states[] = {"PLANNED", "WAITING", "ON_TRACK", "FINISHED", "PERMITTED"};
    char line[LINE_SIZE];
    snprintf(line, sizeof(line),
             "SNAPSHOT time=%05d dir=%s occupancy=%d capacity=%d group=%d finished=%d/%d\n",
             minute, direction_name(railway.direction), railway.occupied,
             railway.config.capacity, railway.group_size, railway.finished, railway.total);
    send_line(slot, line);
    for (int i = 0; i < railway.total; i++) {
        Train *train = &railway.trains[i];
        snprintf(line, sizeof(line),
                 "STATE train=%03d dir=%s type=%s state=%s connected=%d\n",
                 train->id, direction_name(train->direction),
                 train->passenger ? "passenger" : "freight", states[train->state],
                 railway.clients[i] >= 0);
        send_line(slot, line);
    }
}

static void register_peer(int slot, const char *line, int minute) {
    Peer *peer = &railway.peers[slot];
    if (strcmp(line, "WATCH") == 0) {
        peer->role = OBSERVER;
        snapshot(slot, minute);
        return;
    }

    int id;
    if (strncmp(line, "TRAIN ", 6) != 0 || read_number(line + 6, &id) == FAIL ||
        id < 1 || id > railway.total) {
        send_line(slot, "ERROR Некорректный номер поезда или команда\n");
        peer->role = CLOSING;
        return;
    }

    int index = id - 1;
    if (railway.clients[index] >= 0) disconnect(railway.clients[index]);
    railway.clients[index] = slot;
    peer->role = TRAIN;
    peer->train = index;
    Train *train = &railway.trains[index];
    char reply[LINE_SIZE];
    snprintf(reply, sizeof(reply), "REGISTERED train=%03d dir=%s type=%s arrival=%d state=%d\n",
             id, direction_name(train->direction), train->passenger ? "passenger" : "freight",
             train->arrival, train->state);
    send_line(slot, reply);
    event(minute, "REGISTER", index, train->direction, "Клиент поезда подключился");
    if (train->state == PERMITTED) send_line(slot, "GO\n");
    if (train->state == FINISHED) send_line(slot, "DONE\n");
}

// the train takes a spot only after it confirms entry
static void enter(int index, int minute) {
    Train *train = &railway.trains[index];
    if (train->state != PERMITTED || railway.pending != index) return;
    if (railway.occupied >= railway.config.capacity ||
        train->direction != railway.direction ||
        (railway.last_entry >= 0 && minute - railway.last_entry < railway.config.gap)) {
        railway.result = RUN_INVARIANT_ERROR;
        return;
    }

    int waited = minute - train->arrival;
    train->state = ON_TRACK;
    train->entry_time = minute;
    railway.pending = -1;
    railway.occupied++;
    railway.group_size++;
    railway.last_entry = minute;
    railway.admitted++;
    railway.total_wait += waited;
    if (waited > railway.maximum_wait) railway.maximum_wait = waited;
    event(minute, "ENTER", index, train->direction, "Поезд въехал на участок");
    if (waited >= railway.config.max_wait) {
        railway.warnings++;
        event(minute, "WAIT_ALERT", index, train->direction, "Поезд достиг порога ожидания");
    }
}

static void read_peer(int slot) {
    Peer *peer = &railway.peers[slot];
    int result = read_line(peer->fd, &peer->input);
    if (result < 0) {
        if (peer->role == TRAIN) {
            int index = peer->train;
            disconnect(slot);
            event(current_minute(), "DISCONNECT", index, railway.trains[index].direction,
                  "Клиент поезда отключился");
        } else {
            disconnect(slot);
        }
        return;
    }
    if (result == 0 || stop_requested) return;

    int minute = current_minute();
    if (railway.config.until > 0 && minute >= railway.config.until) return;
    const char *line = peer->input.data;
    if (peer->role == NEW) register_peer(slot, line, minute);
    else if (peer->role == TRAIN && strcmp(line, "ENTER") == 0) enter(peer->train, minute);
    else disconnect(slot);
}

static void accept_peer(int listener) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return;
    if (nonblocking(fd) == 0) {
        for (int i = 0; i < MAX_PEERS; i++) {
            if (railway.peers[i].fd < 0) {
                railway.peers[i] = (Peer){.fd = fd, .train = -1, .role = NEW};
                return;
            }
        }
    }
    close(fd);
}

static void clear_track(int minute) {
    if (railway.occupied == 0 && railway.pending < 0 && railway.direction != EMPTY) {
        event(minute, "CLEAR", -1, railway.direction, "Участок освобождён");
        railway.direction = EMPTY;
        railway.group_size = 0;
    }
}

static void update_trains(int minute) {
    for (int i = 0; i < railway.total; i++) {
        Train *train = &railway.trains[i];
        if (train->state == PLANNED && railway.clients[i] >= 0 && minute >= train->arrival) {
            train->state = WAITING;
            event(minute, "ARRIVE", i, train->direction,
                  train->passenger ? "Пассажирский поезд прибыл на станцию" :
                                     "Грузовой поезд прибыл на станцию");
            event(minute, "QUEUE", i, train->direction, "Поезд ожидает разрешения диспетчера");
        }
        if (train->state != ON_TRACK) continue;
        if (!railway.progressed[i] && minute - train->entry_time >= railway.config.travel / 2) {
            railway.progressed[i] = 1;
            event(minute, "PROGRESS", i, train->direction, "Поезд проходит однопутный участок");
        }
        if (minute - train->entry_time >= railway.config.travel) {
            train->state = FINISHED;
            train->exit_time = minute;
            railway.occupied--;
            railway.finished++;
            event(minute, "EXIT", i, train->direction, "Поезд прибыл на другую станцию");
        }
    }
}

static void dispatch(int minute) {
    Config *config = &railway.config;
    if (railway.pending >= 0 && milliseconds() >= railway.permit_deadline) {
        int slot = railway.clients[railway.pending];
        if (slot >= 0) disconnect(slot);
    }
    if (railway.pending >= 0 && railway.clients[railway.pending] < 0) {
        int index = railway.pending;
        railway.trains[index].state = WAITING;
        railway.pending = -1;
        event(minute, "REVOKE", index, railway.trains[index].direction,
              "Разрешение отменено до повторного подключения");
    }

    clear_track(minute);
    if (railway.pending >= 0 || railway.occupied >= config->capacity ||
        (railway.last_entry >= 0 && minute - railway.last_entry < config->gap)) return;

    Train available[MAX_TRAINS];
    memcpy(available, railway.trains, (size_t)railway.total * sizeof(*available));
    for (int i = 0; i < railway.total; i++) {
        if (railway.clients[i] < 0) available[i].state = PLANNED;
    }

    if (railway.direction == EMPTY) {
        railway.direction = choose_direction(available, railway.total, minute, config);
        if (railway.direction == EMPTY) return;
        railway.groups++;
        event(minute, "DIRECTION", -1, railway.direction, "Диспетчер установил направление");
    }

    if (railway.group_size >= config->max_group) return;
    int index = choose_train(available, railway.total, railway.direction, minute, config);
    if (index < 0) return;
    railway.trains[index].state = PERMITTED;
    railway.pending = index;
    railway.permit_deadline = milliseconds() + ACK_TIMEOUT_MS;
    event(minute, "PERMIT", index, railway.direction, "Диспетчер разрешил въезд");
    if (railway.clients[index] >= 0) send_line(railway.clients[index], "GO\n");
}

static void exchange(int listener) {
    struct pollfd fds[MAX_PEERS + 1];
    fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *peer = &railway.peers[i];
        short events = peer->role == CLOSING ? 0 : POLLIN;
        if (peer->start < peer->end) events |= POLLOUT;
        fds[i + 1] = (struct pollfd){.fd = peer->fd, .events = events};
    }

    int step = railway.config.delay_ms ? railway.config.delay_ms : 1;
    if (poll(fds, MAX_PEERS + 1, step < 20 ? step : 20) < 0) {
        if (errno != EINTR) railway.result = RUN_ERROR;
        return;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        short events = fds[i + 1].revents;
        if (railway.peers[i].fd < 0) continue;
        if (events & (POLLIN | POLLHUP)) read_peer(i);
        if (events & (POLLERR | POLLNVAL)) disconnect(i);
        if (events & POLLOUT) flush(i);
    }
    if (!stop_requested && (fds[0].revents & POLLIN)) accept_peer(listener);
}

static const char *reason(RunResult result) {
    switch (result) {
        case RUN_OK: return "completed";
        case RUN_TIME_LIMIT: return "time_limit";
        case RUN_INTERRUPTED: return "interrupted";
        case RUN_INVARIANT_ERROR: return "invariant_error";
        default: return "io_error";
    }
}

// give clients time to receive the stop message
static void finish(int minute) {
    char summary[LINE_SIZE];
    snprintf(summary, sizeof(summary),
             "SUMMARY reason=%s finished=%d/%d admitted=%d groups=%d max_wait=%d avg_wait=%.2f warnings=%d time=%d\n",
             reason(railway.result), railway.finished, railway.total, railway.admitted,
             railway.groups, railway.maximum_wait,
             railway.admitted ? (double)railway.total_wait / railway.admitted : 0.0,
             railway.warnings, minute);
    record(summary);
    write_all(STDOUT_FILENO, summary, strlen(summary));

    struct pollfd fds[MAX_PEERS];
    for (int i = 0; i < MAX_PEERS; i++) {
        if (railway.peers[i].fd < 0) continue;
        if (railway.peers[i].role == OBSERVER) send_line(i, summary);
        send_line(i, "STOP\n");
        railway.peers[i].role = CLOSING;
        railway.peers[i].stopping = 1;
    }
    long long deadline = milliseconds() + 1000;
    while (milliseconds() < deadline) {
        int count = 0;
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *peer = &railway.peers[i];
            short events = POLLIN;
            if (peer->fd >= 0) {
                if (peer->start < peer->end) events |= POLLOUT;
                else if (peer->stopping == 1) {
                    shutdown(peer->fd, SHUT_WR);
                    peer->stopping = 2;
                }
                count++;
            }
            fds[i] = (struct pollfd){.fd = peer->fd, .events = events};
        }
        if (!count) break;
        if (poll(fds, MAX_PEERS, 20) <= 0) continue;
        for (int i = 0; i < MAX_PEERS; i++) {
            if (fds[i].revents & POLLOUT) flush(i);
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                disconnect(i);
            } else if ((fds[i].revents & POLLIN) && railway.peers[i].fd >= 0) {
                char ignored[LINE_SIZE];
                if (read(railway.peers[i].fd, ignored, sizeof(ignored)) == 0) disconnect(i);
            }
        }
    }
    for (int i = 0; i < MAX_PEERS; i++) disconnect(i);
}

int main(int argc, char **argv) {
    struct sockaddr_in address;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        dprintf(STDOUT_FILENO, "Использование: %s IP PORT [параметры]\n", argv[0]);
        usage(argv[0]);
        return RUN_OK;
    }
    if (argc < 3 || make_address(&address, argv[1], argv[2]) < 0 ||
        read_config(argc - 2, argv + 2, &railway.config) == FAIL ||
        railway.config.log_path[0] == '\0') {
        dprintf(STDERR_FILENO, "Использование: %s IP PORT [параметры]. Справка: --help\n", argv[0]);
        return RUN_ERROR;
    }

    setup_signals();
    int listener = listen_tcp(&address);
    if (listener < 0) {
        perror("listen");
        return RUN_ERROR;
    }
    railway.log_fd = open(railway.config.log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (railway.log_fd < 0) {
        perror("log");
        close(listener);
        return RUN_ERROR;
    }
    railway.total = railway.config.left + railway.config.right;
    railway.pending = railway.last_entry = -1;
    railway.direction = EMPTY;
    for (int i = 0; i < MAX_PEERS; i++) railway.peers[i].fd = -1;
    for (int i = 0; i < MAX_TRAINS; i++) railway.clients[i] = -1;
    make_trains(railway.trains, &railway.config);

    clock_gettime(CLOCK_MONOTONIC, &railway.start);
    dprintf(STDOUT_FILENO, "Сервер слушает %s:%s, поездов: %d\n", argv[1], argv[2], railway.total);
    while (railway.finished < railway.total && railway.result == RUN_OK) {
        int minute = current_minute();
        if (stop_requested) {
            railway.result = RUN_INTERRUPTED;
            break;
        }
        if (railway.config.until > 0 && minute >= railway.config.until) {
            railway.result = RUN_TIME_LIMIT;
            break;
        }
        update_trains(minute);
        dispatch(minute);
        if (!valid_state(railway.trains, railway.total, railway.occupied, railway.direction,
                         railway.config.capacity, minute, railway.config.travel)) {
            railway.result = RUN_INVARIANT_ERROR;
            break;
        }
        if (railway.log_error) {
            railway.result = RUN_ERROR;
            break;
        }
        if (railway.finished < railway.total) exchange(listener);
    }
    clear_track(current_minute());
    close(listener);
    finish(current_minute());
    if (close(railway.log_fd) < 0 || railway.log_error) return RUN_ERROR;
    return railway.result;
}
