# devices/main/logging — reconstructed VanMoof log-collector logic (behaviour-oriented C)
# (-D_DEFAULT_SOURCE: models the vendor std::string/ifstream/time helpers over
#  POSIX strtok_r/strdup/localtime_r/strftime.)
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
