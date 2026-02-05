# 001: Stale in_used Counter After Fork

## Issue

When a multi-threaded process calls `fork()` while other threads are executing inside dynarec blocks, the child inherits the `in_used` counters from those threads. Since those threads don't exist in the child, the counters become permanently stale.

## Impact

- Blocks with stale `in_used > 0` can never be purged
- Memory leak proportional to number of threads in parent at fork time
- Only affects: multi-threaded fork WITHOUT exec

## NOT Affected

- Single-threaded programs (deferred fork mechanism protects)
- Fork + exec patterns (exec replaces address space)

## Test Features

This test includes several features to thoroughly demonstrate the bug:

| Feature | Description |
|---------|-------------|
| **Multiple workers** | 8 worker threads (configurable via `NUM_WORKERS`) |
| **Multiple hot functions** | 4 different functions = 4 different dynarec blocks |
| **Diagnostics table** | Shows expected `in_used` values for each block |
| **Stress test mode** | Multiple sequential forks with grandchild processes |
| **Memory pressure** | Child simulates allocation to trigger purge attempts |

## Configuration

Edit these constants in `main.c` to adjust the test:

```c
#define NUM_WORKERS       8    /* Number of worker threads */
#define NUM_HOT_FUNCS     4    /* Number of different hot functions */
#define STRESS_FORKS      5    /* Number of forks in stress test mode */
#define COMPILE_WAIT_MS   300  /* Time to wait for dynarec compilation */
```

## Related Files in Box64

| File | Description |
|------|-------------|
| `src/dynarec/dynablock_private.h:18` | `in_used` field definition |
| `src/custommem.c:1630` | PurgeDynarecMap checks `in_used == 0` |
| `src/custommem.c:2929` | `atfork_child_custommem()` - missing reset |

## Build

```bash
make
```

Or from repo root:

```bash
make 001_fork_in_used_leak
```

## Run

### Normal mode (single fork, 8 threads)

```bash
BOX64_DYNAREC=1 BOX64_LOG=1 box64 ./001_fork_in_used_leak
```

### Stress test mode (multiple forks)

```bash
BOX64_DYNAREC=1 box64 ./001_fork_in_used_leak --stress
```

### With full dynarec logging

```bash
BOX64_DYNAREC=1 BOX64_DYNAREC_LOG=3 box64 ./001_fork_in_used_leak
```

## Expected Output

### Normal Mode

```
########################################
 TEST 001: Stale in_used After Fork
 Configuration: 8 workers, 4 hot functions
########################################

[Main] Creating 8 worker threads...
[Worker 0] Using hot_compute_0 (dynarec block 0)
[Worker 1] Using hot_compute_1 (dynarec block 1)
...

[Diagnostics] Expected in_used state at fork (parent):
  +-----------------+------------------+
  | Dynarec Block   | Expected in_used |
  +-----------------+------------------+
  | hot_compute_0   |                2 |
  | hot_compute_1   |                2 |
  | hot_compute_2   |                2 |
  | hot_compute_3   |                2 |
  +-----------------+------------------+
  | TOTAL STALE     |                8 |
  +-----------------+------------------+

========================================
 CHILD PROCESS (PID 12345) - Fork #1
========================================

State after fork:
  - Inherited 4 dynarec blocks from parent
  - Parent had 8 worker threads inside these blocks
  - Child has 0 worker threads
  - All inherited in_used counters are STALE!
```

### Stress Test Mode

Shows multiple forks with accumulating stale counters, including grandchild processes that inherit already-stale counters.

## Diagnostic Patch

To see actual `in_used` values, apply the diagnostic patch in the `patches/` directory to Box64.
