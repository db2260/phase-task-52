CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O1

all: debounce

debounce: mode_switch_handler.c
	$(CC) $(CFLAGS) -o $@ $< -lpthread

run: debounce
	./debounce

clean:
	rm -f debounce

.PHONY: all run clean
