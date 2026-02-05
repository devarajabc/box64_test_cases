# 002_0f00_missing_braces Makefile

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

TARGET = 002_0f00_missing_braces
BIN_DIR ?= .

SRCS = main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)
