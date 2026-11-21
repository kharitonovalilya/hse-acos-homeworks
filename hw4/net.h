#ifndef NET_H
#define NET_H

#include <netinet/in.h>
#include <stddef.h>

#define LINE_SIZE 512

typedef struct {
    char data[LINE_SIZE];
    size_t length;
} Line;

int make_address(struct sockaddr_in *address, const char *ip, const char *port);
int listen_tcp(const struct sockaddr_in *address);
int connect_tcp(const struct sockaddr_in *address);
int nonblocking(int fd);
int write_all(int fd, const char *data, size_t length);
int read_line(int fd, Line *line);
void setup_signals(void);

#endif
