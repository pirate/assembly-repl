CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g

.PHONY: all clean run

all: asmrepl

asmrepl: src/asmrepl.c
	$(CC) $(CFLAGS) $< -o $@

run: asmrepl
	./asmrepl

clean:
	rm -rf asmrepl .asmrepl-build
