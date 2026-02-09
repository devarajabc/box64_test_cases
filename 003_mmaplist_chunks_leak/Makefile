# 003_mmaplist_chunks_leak Makefile

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=

TARGET = 003_mmaplist_chunks_leak
BIN_DIR ?= .

SRCS = main.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)
