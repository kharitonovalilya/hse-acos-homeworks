#include "net.h"
#include "railway.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

volatile sig_atomic_t stop_requested = 0;

static void stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

void setup_signals(void) {
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int make_address(struct sockaddr_in *address, const char *ip, const char *port) {
    int number;
    if (read_number(port, &number) == FAIL || number < 1 || number > 65535) {
        return -1;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((unsigned short)number);
    return inet_pton(AF_INET, ip, &address->sin_addr) == 1 ? 0 : -1;
}

int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int listen_tcp(const struct sockaddr_in *address) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    if (fd < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0 &&
        bind(fd, (const struct sockaddr *)address, sizeof(*address)) == 0 &&
        listen(fd, 128) == 0 && nonblocking(fd) == 0) return fd;

    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int connect_tcp(const struct sockaddr_in *address) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0) return fd;

    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t count = write(fd, data, length);
        if (count < 0 && errno == EINTR && !stop_requested) continue;
        if (count <= 0) return -1;
        data += count;
        length -= (size_t)count;
    }
    return 0;
}

int read_line(int fd, Line *line) {
    while (line->length < sizeof(line->data) - 1) {
        char ch;
        ssize_t count = read(fd, &ch, 1);
        if (count == 0) return -1;
        if (count < 0) {
            if (errno == EINTR && !stop_requested) continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
        if (ch == '\n') {
            line->data[line->length] = '\0';
            line->length = 0;
            return 1;
        }
        if (ch == '\0') return -1;
        line->data[line->length++] = ch;
    }
    return -1;
}
