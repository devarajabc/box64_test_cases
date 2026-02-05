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

```bash
BOX64_DYNAREC=1 BOX64_LOG=1 box64 ./001_fork_in_used_leak
```

## Expected Output

```
========================================
 CHILD PROCESS (PID 12345)
========================================

State after fork:
  - Inherited hot_compute's dynarec block
  - Block has in_used >= 1 (from parent's worker)
  - Child has NO worker thread
  - in_used is STALE - will never decrement!
```

## Diagnostic Patch

To see actual `in_used` values, apply the diagnostic patch in the `patches/` directory to Box64.
