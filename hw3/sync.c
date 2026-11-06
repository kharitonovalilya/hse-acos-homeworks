#define _POSIX_C_SOURCE 200809L
#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

int notify_init(Notification *notification, const char *session, int index, int value) {
#ifdef NAMED
    snprintf(notification->name, sizeof notification->name,
             "/acos27_%s_s%d", session, index);
    sem_t *semaphore = sem_open(notification->name, O_CREAT | O_EXCL, 0600, value);
    if (semaphore == SEM_FAILED) return -1;
    return sem_close(semaphore);
#else
    (void)session;
    (void)index;
    return sem_init(&notification->semaphore, 1, (unsigned)value);
#endif
}

int notify_post(Notification *notification) {
#ifdef NAMED
    sem_t *semaphore = sem_open(notification->name, 0);
    if (semaphore == SEM_FAILED) return -1;
    int result = sem_post(semaphore);
    sem_close(semaphore);
    return result;
#else
    return sem_post(&notification->semaphore);
#endif
}

int wait_event(Notification *notification) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 10000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
#ifdef NAMED
    sem_t *semaphore = sem_open(notification->name, 0);
    if (semaphore == SEM_FAILED) return -1;
#else
    sem_t *semaphore = &notification->semaphore;
#endif
    int result;
    do {
        result = sem_timedwait(semaphore, &deadline);
    } while (result != 0 && errno == EINTR);
    int saved = errno;
#ifdef NAMED
    sem_close(semaphore);
#endif
    if (result == 0) return 1;
    return saved == ETIMEDOUT ? 0 : -1;
}

void notify_destroy(Notification *notification) {
#ifdef NAMED
    sem_unlink(notification->name);
#else
    sem_destroy(&notification->semaphore);
#endif
}
