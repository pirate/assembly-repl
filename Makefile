CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
UNAME_S := $(shell uname -s)
CODE_REPL_LIBS :=
ifeq ($(UNAME_S),Linux)
CODE_REPL_LIBS := -ldl
endif

.PHONY: all clean run

all: asmrepl language-repl

asmrepl: src/asmrepl.c
	$(CC) $(CFLAGS) $< -o $@

language-repl: src/language-repl.c
	$(CC) $(CFLAGS) $< -o $@ $(CODE_REPL_LIBS)

run: asmrepl
	./asmrepl

clean:
	rm -rf asmrepl language-repl .repl-build
