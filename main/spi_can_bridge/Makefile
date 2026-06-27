# devices/main/spi_can_bridge — reconstructed VanMoof SPI<->CAN bridge (behaviour-oriented C)
# (-D_DEFAULT_SOURCE: the SocketCAN endpoint uses socket/ioctl/bind/struct ifreq.)
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
