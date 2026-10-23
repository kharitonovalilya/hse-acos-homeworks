#ifndef SYNC_H
#define SYNC_H

#ifdef USE_COND
#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int count;
} Notification;
#else
#include <semaphore.h>
typedef sem_t Notification;
#endif

int notify_init(Notification *notification);
void notify_destroy(Notification *notification);
void notify_post(Notification *notification);
int wait_event(Notification *notification);

#endif
