CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS = -pthread -lrt
SOURCES = config.c simulation.c sync.c ipc.c
HEADERS = railway.h model.h sync.h

.PHONY: all test examples clean
all: railway railway_named railway_independent railway_queue

railway: main.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) main.c $(SOURCES) $(LDLIBS) -o $@

railway_named: main.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -DNAMED main.c $(SOURCES) $(LDLIBS) -o $@

railway_independent: main.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -DNAMED -DINDEPENDENT main.c $(SOURCES) $(LDLIBS) -o $@

railway_queue: main.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -DNAMED -DINDEPENDENT -DQUEUE main.c $(SOURCES) $(LDLIBS) -o $@

tests: tests.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) tests.c $(SOURCES) $(LDLIBS) -o $@

tests_named: tests.c $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -DNAMED tests.c $(SOURCES) $(LDLIBS) -o $@

test: all tests tests_named
	sh ./run_tests.sh

examples: all
	sh ./run_examples.sh

clean:
	rm -f railway railway_named railway_independent railway_queue tests tests_named
