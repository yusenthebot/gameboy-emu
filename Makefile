# Game Boy emulator - build
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-initializer-overrides -g
BIN     := gbemu
PLAY    := gbplay

# Core sources shared by both binaries (everything except the two entry points).
CORE    := $(filter-out src/main.c src/play.c, $(wildcard src/*.c))
COREOBJ := $(CORE:.c=.o)

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

.PHONY: all clean test play

all: $(BIN)

# Headless test/dump harness (used by tools/run_tests.sh).
$(BIN): $(COREOBJ) src/main.o
	$(CC) $(CFLAGS) -o $@ $^

# Interactive SDL2 frontend.
play: $(PLAY)
$(PLAY): $(COREOBJ) src/play.o
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS)

src/play.o: src/play.c src/gb.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

src/%.o: src/%.c src/gb.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(COREOBJ) src/main.o src/play.o $(BIN) $(PLAY)

# Quick smoke test: run Blargg cpu_instrs and report.
test: $(BIN)
	./$(BIN) roms/cpu_instrs.gb
