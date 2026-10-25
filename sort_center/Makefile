CC ?= gcc
CFLAGS ?= -Wall -Wextra -pedantic -std=c11 -O2 -I./src
TARGET = sort_center
BUILD_DIR = build
SRC_DIR = src

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
HEADERS = $(wildcard $(SRC_DIR)/*.h)

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -pthread -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

run: $(TARGET)
	./$(TARGET) -n 20 --delay 20

test: $(TARGET)
	bash scripts/run_tests.sh "$(abspath $(TARGET))"
