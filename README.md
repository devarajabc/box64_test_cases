# Box64 Test Cases

A collection of test programs to verify Box64 behavior, reproduce bugs, and validate fixes.

## Overview

These are x86_64 Linux programs designed to be run under [Box64](https://github.com/ptitSeb/box64) on ARM64/RV64/LA64 systems.

## Building

### Prerequisites

- x86_64 Linux environment (native, Docker, or cross-compiler)
- GCC with pthread support

### Build All Tests

```bash
make all
```

### Build Specific Test

```bash
make 001_fork_in_used_leak
```

### Using Docker (from Mac/Windows/ARM)

```bash
make docker-build
```

## Test Categories

| Prefix | Category |
|--------|----------|
| 001-099 | Memory management / Dynarec |
| 100-199 | Threading / Synchronization |
| 200-299 | Signals / Exception handling |
| 300-399 | ELF loading / Libraries |
| 400-499 | Syscalls |
| 500-599 | Performance / Stress tests |

## Test List

| ID | Name | Description | Status |
|----|------|-------------|--------|
| 001 | fork_in_used_leak | Stale dynablock `in_used` after fork() | Open |

## Running Tests

On ARM64 Linux with Box64:

```bash
BOX64_DYNAREC=1 BOX64_LOG=1 box64 ./bin/001_fork_in_used_leak
```

## Contributing

1. Create a new directory: `NNN_test_name/`
2. Add source files and a local `Makefile`
3. Add entry to the test list above
4. Update root `Makefile`

## License

MIT - Same as Box64
