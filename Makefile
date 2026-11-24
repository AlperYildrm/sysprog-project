CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS =

TARGET  = watchdog
WORKER  = worker

SRCS    = src/main.c src/signal_manager.c src/process_manager.c src/logger.c
OBJS    = $(SRCS:.c=.o)
HEADERS = $(wildcard include/*.h)

.PHONY: all clean run test

all: $(TARGET) $(WORKER)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(WORKER): workers/worker.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Rebuild objects when any header changes
src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

run: all
	./$(TARGET)

test: all
	./tests/test_scenarios.sh

clean:
	rm -f $(OBJS) $(TARGET) $(WORKER) watchdog.log
