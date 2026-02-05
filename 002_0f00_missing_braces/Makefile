# 002_0f00_missing_braces Makefile
# Compile as 32-bit to test the missing braces bug in 32-bit mode

CC ?= gcc
CFLAGS ?= -m32 -O2 -Wall -Wextra
LDFLAGS ?= -m32

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
