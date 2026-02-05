# Box64 Test Cases - Root Makefile

CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -pthread

# Output directory for binaries
BIN_DIR = bin

# List of all test directories
TESTS = 001_fork_in_used_leak

.PHONY: all clean docker-build $(TESTS)

all: $(BIN_DIR) $(TESTS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Build individual tests
001_fork_in_used_leak: $(BIN_DIR)
	$(MAKE) -C $@ BIN_DIR=../$(BIN_DIR)

clean:
	rm -rf $(BIN_DIR)
	@for dir in $(TESTS); do \
		$(MAKE) -C $$dir clean 2>/dev/null || true; \
	done

# Docker build for cross-compilation from Mac/Windows
docker-build:
	docker run --rm -v $(PWD):/src -w /src gcc:latest make all

# Docker build with static linking (more portable)
docker-build-static:
	docker run --rm -v $(PWD):/src -w /src gcc:latest make all LDFLAGS="-pthread -static"

# List all tests
list:
	@echo "Available tests:"
	@for dir in $(TESTS); do \
		echo "  $$dir"; \
	done
