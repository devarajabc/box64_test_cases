# 004_atfork_thread_safety Makefile

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -pthread

TARGET = 004_atfork_thread_safety
BIN_DIR ?= .

SRCS = main.c

.PHONY: all clean

all: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
