# 004_atfork_thread_safety

## Issue

In box64, `my_pthread_atfork` (wrappedlibpthread.c) and `my___register_atfork` (wrappedlibc.c) both modify the shared `my_context->atforks` array and `atfork_sz`/`atfork_cap` without any locking.

When multiple threads register atfork handlers concurrently, race conditions can cause:

1. **Lost handlers** — two threads read the same `atfork_sz`, both write to the same slot
2. **Heap corruption** — two threads call `realloc()` on the same pointer simultaneously
3. **Corrupted function pointers** — partial writes to `atforks[i]` struct fields

## Test Design

- 8 threads each register 16 atfork handlers simultaneously (128 total)
- A pthread barrier ensures all threads start registration at the same instant
- After registration, fork() is called multiple times
- Each handler increments an atomic counter
- Expected vs actual handler counts are compared

## Expected Results

**Correct implementation**: All 128 handlers fire in each fork (PASS)

**Buggy implementation**: Fewer handlers fire, or crash during registration/fork (FAIL)

## Usage

```bash
# Cross-compile for x86_64
make CC=x86_64-linux-gnu-gcc

# Run under box64
box64 ./004_atfork_thread_safety

# More rounds for higher race probability
box64 ./004_atfork_thread_safety --rounds 20
```
