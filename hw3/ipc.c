#define _POSIX_C_SOURCE 200809L
#include "model.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef QUEUE
#include <mqueue.h>
#endif

static char object_name[96];
static Railway *model;
static int owner, initialized, mutex_ready, changed_ready, object_created;
static Notification *gate;
#ifdef NAMED
static Notification named_gate;
#endif
static pid_t children[MAX_TRAINS + 1];
static int child_count;
#ifdef QUEUE
static mqd_t queue = (mqd_t)-1;
#endif

static void pause_short(void) {
    struct timespec delay = {0, 10000000L};
    nanosleep(&delay, NULL);
}

static int valid_session(const char *session) {
    size_t length = strlen(session);
    if (length == 0 || length > 40) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static void cleanup(void) {
    if (getpid() != owner) return;
    // Reap every child before releasing objects used by these processes.
    for (int i = 0; i < child_count; i++) {
        if (children[i] > 0) kill(children[i], SIGTERM);
    }
    for (int i = 0; i < child_count; i++) {
        if (children[i] <= 0) continue;
        kill(children[i], SIGKILL);
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
    }
    if (model != NULL) {
        for (int i = 0; i < initialized; i++) notify_destroy(&model->permits[i]);
        if (changed_ready) notify_destroy(&model->changed);
        if (mutex_ready) notify_destroy(gate);
    }
#ifdef QUEUE
    if (queue != (mqd_t)-1) mq_close(queue);
    if (object_created) mq_unlink(object_name);
    free(model);
#else
    if (model != NULL) munmap(model, sizeof *model);
    if (object_created) shm_unlink(object_name);
#endif
    model = NULL;
    object_created = mutex_ready = changed_ready = initialized = child_count = 0;
}

#ifdef QUEUE
// The queue contains one model snapshot split into <= 10 messages.
// The named binary semaphore grants exclusive ownership of that snapshot.
static int transfer(Railway *railway, int write_state) {
    char *bytes = (char *)railway;
    for (size_t offset = 0; offset < sizeof *railway; offset += 8192) {
        size_t size = sizeof *railway - offset;
        if (size > 8192) size = 8192;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 2;
        int result;
        do {
            if (write_state) result = mq_timedsend(queue, bytes + offset, size, 0, &deadline);
            else {
                char buffer[8192];
                ssize_t count = mq_timedreceive(queue, buffer, sizeof buffer, NULL, &deadline);
                result = count == (ssize_t)size ? 0 : -1;
                if (result == 0) memcpy(bytes + offset, buffer, size);
            }
        } while (result < 0 && errno == EINTR);
        if (result < 0) return -1;
    }
    return 0;
}
#endif

void model_lock(Railway *railway) {
    int result = 0;
    for (int tries = 0; tries < 300 && result == 0; tries++) {
        result = wait_event(gate);
    }
    if (result != 1) {
        fprintf(stderr, "Не удалось получить доступ к модели.\n");
        exit(RUN_ERROR);
    }
#ifdef QUEUE
    if (transfer(railway, 0) < 0) {
        notify_post(gate);
        exit(RUN_ERROR);
    }
#endif
    if (!railway->stopped && stop_requested) stop_all(railway, RUN_INTERRUPTED);
}

void model_unlock(Railway *railway) {
#ifdef QUEUE
    if (transfer(railway, 1) < 0) {
        notify_post(gate);
        exit(RUN_ERROR);
    }
#else
    (void)railway;
#endif
    if (notify_post(gate) < 0) exit(RUN_ERROR);
}

static int connect_model(const char *session, int create) {
    if (!valid_session(session)) return -1;
    snprintf(object_name, sizeof object_name, "/acos27_%s", session);
#ifdef QUEUE
    struct mq_attr attributes = {0};
    attributes.mq_maxmsg = 10;
    attributes.mq_msgsize = 8192;
    if (sizeof(Railway) > 10 * 8192) return -1;
    queue = mq_open(object_name, create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR,
                    0600, &attributes);
    if (queue == (mqd_t)-1) return -1;
    if (create) object_created = 1;
    model = calloc(1, sizeof *model);
    if (model == NULL) return -1;
#else
    int fd = shm_open(object_name, create ? O_RDWR | O_CREAT | O_EXCL : O_RDWR, 0600);
    if (fd < 0) return -1;
    if (create) object_created = 1;
    struct stat status;
    if ((create && ftruncate(fd, sizeof *model) < 0) ||
        fstat(fd, &status) < 0 || status.st_size != (off_t)sizeof *model) {
        close(fd);
        return -1;
    }
    void *address = mmap(NULL, sizeof *model, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (address == MAP_FAILED) return -1;
    model = address;
#endif
    if (!create) {
#ifdef NAMED
        gate = &named_gate;
        snprintf(gate->name, sizeof gate->name, "/acos27_%s_s0", session);
#else
        gate = &model->mutex;
#endif
        model_lock(model);
        int ready = model->ready;
        model_unlock(model);
        if (!ready) return -1;
    }
    return 0;
}

static RunResult finish_train(Railway *railway, int index) {
    train_process(railway, index);
    model_lock(railway);
    if (log_error && !railway->stopped) stop_all(railway, RUN_ERROR);
    RunResult result = railway->result;
    if (railway->workers[index] == (int)getpid()) railway->workers[index] = 0;
    model_unlock(railway);
    return result;
}

static void summary(Railway *railway) {
    const char *reason = "все поезда завершили путь";
    if (railway->result == RUN_INTERRUPTED) reason = "прервано пользователем";
    else if (railway->result == RUN_TIME_LIMIT) reason = "достигнут предел времени";
    else if (railway->result != RUN_OK) reason = "ошибка выполнения";
    char message[512];
    snprintf(message, sizeof message,
             "Итог: %s; завершено %d/%d; осталось %d; групп %d; среднее ожидание %.2f мин; максимум %d мин; достигли порога %d",
             reason, railway->finished, railway->total, railway->total - railway->finished,
             railway->groups, railway->admitted ? (double)railway->total_wait / railway->admitted : 0.0,
             railway->maximum_wait, railway->warnings);
    event(current_minute(railway), "SUMMARY", 0, railway->direction, railway->occupied, message);
}

RunResult run_server(const Config *config, const char *session, int independent) {
    owner = (int)getpid();
    // Registered once per process; tests can call simulate several times.
    static int registered;
    if (!registered) {
        atexit(cleanup);
        registered = 1;
    }
    if (connect_model(session, 1) < 0) {
        perror("Не удалось создать IPC (проверьте имя сессии)");
        cleanup();
        return RUN_ERROR;
    }
    model->config = *config;
    model->config.log_path = NULL; // Pointers cannot be passed between independent processes.
    if (config->log_path != NULL) {
        if (strlen(config->log_path) >= sizeof model->log_path || config->log_path[0] != '/') {
            fprintf(stderr, "Для независимых процессов нужен абсолютный путь лога.\n");
            cleanup();
            return RUN_ERROR;
        }
        // Create the log only after this process has claimed the session.
        log_fd = open(config->log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (log_fd < 0) {
            perror("Не удалось открыть лог");
            cleanup();
            return RUN_ERROR;
        }
        snprintf(model->log_path, sizeof model->log_path, "%s", config->log_path);
    }
    model->total = config->left + config->right;
    model->direction = EMPTY;
    model->last_entry = -1;
    make_trains(model->trains, config);
    if (notify_init(&model->mutex, session, 0, 0) < 0) goto failure;
    gate = &model->mutex;
    mutex_ready = 1;
    if (notify_init(&model->changed, session, 1, 0) < 0) goto failure;
    changed_ready = 1;
    for (; initialized < model->total; initialized++) {
        if (notify_init(&model->permits[initialized], session, initialized + 2, 0) < 0)
            goto failure;
    }
    clock_gettime(CLOCK_MONOTONIC, &model->start);
    model->ready = 1;
    log_error = 0;
    char message[256];
    snprintf(message, sizeof message,
             "Старт: поездов %d; вместимость %d; интервал %d; группа %d; стратегия %s; seed %d; session %s; pid %d",
             model->total, config->capacity, config->gap, config->max_group,
             config->priority ? "priority" : "fifo", config->seed, session, owner);
    event(0, "CONFIG", 0, EMPTY, 0, message);
#ifdef QUEUE
    if (transfer(model, 1) < 0) goto failure;
#endif
    // Publish the complete model before allowing any client to read it.
    if (notify_post(gate) < 0) goto failure;
    if (independent) {
        dispatcher_process(model);
        // Peers observe stopped/finished state before IPC names disappear.
        for (int tries = 0; tries < 200; tries++) {
            model_lock(model);
            int active = 0;
            for (int i = 0; i < model->total; i++) {
                if (model->workers[i] != 0) active++;
            }
            model_unlock(model);
            if (active == 0) break;
            pause_short();
        }
    } else {
        int total = model->total;
        for (int i = 0; i <= total; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                model_lock(model);
                stop_all(model, RUN_ERROR);
                model_unlock(model);
                break;
            }
            if (pid == 0) {
                if (i < total) finish_train(model, i);
                else dispatcher_process(model);
                _exit(log_error ? RUN_ERROR : RUN_OK);
            }
            children[child_count++] = pid;
        }
        int remaining = child_count;
        while (remaining > 0) {
            model_lock(model);
            for (int i = 0; i < child_count; i++) {
                if (children[i] <= 0) continue;
                int status;
                pid_t result = waitpid(children[i], &status, WNOHANG);
                if (result == children[i]) {
                    children[i] = 0;
                    remaining--;
                    if (!model->stopped &&
                        (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
                         (i < total && model->trains[i].state != FINISHED) ||
                         (i == total && model->finished != total))) stop_all(model, RUN_ERROR);
                }
            }
            model_unlock(model);
            if (remaining > 0) pause_short();
        }
    }
    model_lock(model);
    summary(model);
    RunResult result = model->result;
    model_unlock(model);
    cleanup();
    if (log_error) return RUN_ERROR;
    return result;

failure:
    perror("Не удалось настроить IPC");
    cleanup();
    return RUN_ERROR;
}

RunResult simulate(const Config *config) {
    char session[48];
    snprintf(session, sizeof session, "fork_%d", (int)getpid());
    return run_server(config, session, 0);
}

RunResult run_train(const char *session, int id) {
    if (connect_model(session, 0) < 0) {
        fprintf(stderr, "Сессия ещё не готова или не существует. Сначала запустите сервер.\n");
        return RUN_ERROR;
    }
    model_lock(model);
    int valid = id >= 1 && id <= model->total && !model->stopped &&
                model->workers[id - 1] == 0 && model->trains[id - 1].state == PLANNED;
    RunResult result = RUN_ERROR;
    if (valid) {
        if (model->log_path[0] != '\0') {
            log_fd = open(model->log_path, O_WRONLY | O_APPEND);
            if (log_fd < 0) {
                perror("Не удалось открыть лог поезда");
                stop_all(model, RUN_ERROR);
                valid = 0;
            }
        }
        // Register only after this train can write its events.
        if (valid) model->workers[id - 1] = (int)getpid();
    } else {
        fprintf(stderr, "Неверный или уже подключённый номер поезда.\n");
    }
    model_unlock(model);
    if (valid) result = finish_train(model, id - 1);
    if (log_fd >= 0) close(log_fd);
#ifdef QUEUE
    mq_close(queue);
    free(model);
#else
    munmap(model, sizeof *model);
#endif
    return log_error ? RUN_ERROR : result;
}
