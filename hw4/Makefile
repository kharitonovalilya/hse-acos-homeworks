CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2

all: railway_server railway_train railway_observer

railway_server: server.c simulation.c config.c net.c railway.h net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ server.c simulation.c config.c net.c

railway_train: client.c config.c net.c railway.h net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -DTRAIN_CLIENT -o $@ client.c config.c net.c

railway_observer: client.c config.c net.c railway.h net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ client.c config.c net.c

tests: tests.c server.c simulation.c config.c net.c railway.h net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests.c simulation.c config.c net.c

test: all tests
	./run_tests.sh

clean:
	rm -f railway_server railway_train railway_observer tests railway.log

.PHONY: all test clean
