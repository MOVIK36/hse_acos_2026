CC ?= gcc
CFLAGS ?= -Wall -Wextra -pedantic -std=c11 -O2 -I./src
TARGET = sort_center
CLIENTS = sort_source sort_observer
BUILD_DIR = build
SRC_DIR = src

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
HEADERS = $(wildcard $(SRC_DIR)/*.h)

.PHONY: all clean run test

all: $(TARGET) $(CLIENTS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^ -lrt

sort_source: $(BUILD_DIR)/clients/source.o $(BUILD_DIR)/tcp.o
	$(CC) $(CFLAGS) -o $@ $^

sort_observer: $(BUILD_DIR)/clients/observer.o $(BUILD_DIR)/tcp.o
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -pthread -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(CLIENTS)

run: $(TARGET)
	./$(TARGET) -n 20 --delay 20

test: all
	timeout -k 5 90 bash scripts/run_tests.sh "$(abspath $(TARGET))"
	timeout -k 5 90 bash scripts/run_tcp_tests.sh "$(abspath .)"
