# 003_mmaplist_chunks_leak Makefile
#
# Builds two artifacts:
#   1. libhot.so  - x86_64 shared library (dlopen target)
#   2. 003_mmaplist_chunks_leak - x86_64 binary (test driver)

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -ldl

TARGET = 003_mmaplist_chunks_leak
LIB = libhot.so
BIN_DIR ?= .

.PHONY: all clean

all: $(BIN_DIR)/$(TARGET) $(BIN_DIR)/$(LIB)

$(BIN_DIR)/$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/$(LIB): libhot.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

clean:
	rm -f $(TARGET) $(LIB)
