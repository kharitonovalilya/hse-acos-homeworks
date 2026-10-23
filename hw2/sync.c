#define _POSIX_C_SOURCE 200809L
#include "sync.h"

#include <errno.h>
#include <time.h>

int notify_init(Notification *notification) {
#ifdef USE_COND
    notification->count = 0;
    if (pthread_mutex_init(&notification->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&notification->condition, NULL) != 0) {
        pthread_mutex_destroy(&notification->mutex);
        return -1;
    }
    return 0;
#else
    return sem_init(notification, 0, 0);
#endif
}

void notify_destroy(Notification *notification) {
#ifdef USE_COND
    pthread_cond_destroy(&notification->condition);
    pthread_mutex_destroy(&notification->mutex);
#else
    sem_destroy(notification);
#endif
}

void notify_post(Notification *notification) {
#ifdef USE_COND
    pthread_mutex_lock(&notification->mutex);
    notification->count++;
    pthread_cond_signal(&notification->condition);
    pthread_mutex_unlock(&notification->mutex);
#else
    sem_post(notification);
#endif
}

// a short timeout also lets the dispatcher notice Ctrl+C.
int wait_event(Notification *notification) {
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_nsec += 10000000L;
    if (limit.tv_nsec >= 1000000000L) {
        limit.tv_sec++;
        limit.tv_nsec -= 1000000000L;
    }
#ifdef USE_COND
    int result = 0;
    pthread_mutex_lock(&notification->mutex);
    // Check the counter again after a wakeup
    while (notification->count == 0 && result == 0) {
        result = pthread_cond_timedwait(&notification->condition,
                                        &notification->mutex, &limit);
    }
    if (notification->count > 0) {
        notification->count--;
        result = 0;
    }
    pthread_mutex_unlock(&notification->mutex);
    if (result == 0) return 1;
    if (result == ETIMEDOUT) return 0;
    return -1;
#else
    if (sem_timedwait(notification, &limit) == 0) return 1;
    if (errno == ETIMEDOUT || errno == EINTR) return 0;
    return -1;
#endif
}
