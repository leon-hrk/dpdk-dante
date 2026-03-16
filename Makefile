CC = gcc
CFLAGS = -O3 -Wall -MMD $(shell pkg-config --cflags libdpdk)
LDFLAGS = $(shell pkg-config --libs libdpdk)

SRC_DIR = src
BUILD_DIR = build

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/forwarder.c \
       $(SRC_DIR)/trace_timewheel.c \
       $(SRC_DIR)/delay_timewheel.c \
       $(SRC_DIR)/vqueue_ring.c \
       $(SRC_DIR)/logging.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)
TARGET = dpdk-dante

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

-include $(DEPS)

format:
	clang-format -i $(SRC_DIR)/*.c $(SRC_DIR)/*.h

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)

.PHONY: clean format