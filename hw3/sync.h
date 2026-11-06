#ifndef SYNC_H
#define SYNC_H

#include <semaphore.h>

typedef struct {
#ifdef NAMED
    char name[96];
#else
    sem_t semaphore;
#endif
} Notification;

int notify_init(Notification *notification, const char *session, int index, int value);
int notify_post(Notification *notification);
int wait_event(Notification *notification);
void notify_destroy(Notification *notification);

#endif
