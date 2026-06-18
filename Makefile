# Game Boy emulator - build
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-initializer-overrides -g
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := gbemu

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c src/gb.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

# Quick smoke test: run Blargg cpu_instrs and report.
test: $(BIN)
	./$(BIN) roms/cpu_instrs.gb
