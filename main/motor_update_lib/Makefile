# devices/main/motor_update_lib — reconstructed VanMoof TI C2000 SCI flasher (behaviour-oriented C)
# (-D_DEFAULT_SOURCE: the flasher models the vendor serial/gpio transport over
#  POSIX termios/nanosleep/select, which need the default feature set.)
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -Iinclude
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))

all: $(OBJ)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
