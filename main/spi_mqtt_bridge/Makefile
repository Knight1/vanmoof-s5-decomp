# devices/main/spi_mqtt_bridge — reconstructed VanMoof SPI<->MQTT bridge (behaviour-oriented C)
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
