CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test clean

all: railway

railway: main.c config.c simulation.c railway.h
	$(CC) $(CFLAGS) main.c config.c simulation.c -o railway

tests: tests.c config.c simulation.c railway.h
	$(CC) $(CFLAGS) tests.c config.c simulation.c -o tests

test: railway tests
	./run_tests.sh

examples: railway
	./run_examples.sh

clean:
	rm -f railway tests
