CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread

.PHONY: all test examples clean

all: railway railway_cond

railway: main.c config.c simulation.c sync.c railway.h sync.h
	$(CC) $(CFLAGS) main.c config.c simulation.c sync.c -o railway

railway_cond: main.c config.c simulation.c sync.c railway.h sync.h
	$(CC) $(CFLAGS) -DUSE_COND main.c config.c simulation.c sync.c -o railway_cond

tests: tests.c config.c simulation.c sync.c railway.h sync.h
	$(CC) $(CFLAGS) tests.c config.c simulation.c sync.c -o tests

tests_cond: tests.c config.c simulation.c sync.c railway.h sync.h
	$(CC) $(CFLAGS) -DUSE_COND tests.c config.c simulation.c sync.c -o tests_cond

test: all tests tests_cond
	sh ./run_tests.sh

examples: all
	sh ./run_examples.sh

clean:
	rm -f railway railway_cond tests tests_cond
