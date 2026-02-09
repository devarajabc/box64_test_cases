# 003: mmaplist_t->chunks Pointer Array Leak

## Bug Summary

`mmaplist_t->chunks` is a dynamically allocated pointer array (via `box_realloc` in
`MmaplistAddBlock` and `MmaplistAddNBlocks`). It is never freed in two locations:

1. **`fini_custommem_helper()`** (L3129-3137): `free(head)` frees the `mmaplist_t`
   struct but not `head->chunks`. Additionally, `box_free(mmaplist)` at L3137 is
   dead code since `mmaplist` was set to `NULL` at L3130.

2. **`DelMmaplist()`** (L1547-1569): `box_free(list)` frees the struct but not
   `list->chunks`. This function is called during runtime (mapping cleanup), not
   just at shutdown.

## The Fix

Add `box_free(head->chunks)` / `box_free(list->chunks)` before freeing the struct.
Remove the dead `box_free(mmaplist)` line.

See: `patches/003_fix_mmaplist_chunks_leak.patch`

## How to Verify with Valgrind

```bash
# Build the test binary (x86_64)
make CC=x86_64-linux-gnu-gcc

# Build box64 WITHOUT fix (from upstream main)
cd /tmp && git clone --depth 1 https://github.com/ptitSeb/box64.git
cd box64 && mkdir build && cd build
cmake .. -D ARM_DYNAREC=ON -D CMAKE_BUILD_TYPE=Debug
make -j$(nproc) && sudo make install

# Run under valgrind -> should show leak
valgrind --leak-check=full --show-leak-kinds=definite \
  box64 ./003_mmaplist_chunks_leak 2>&1 | grep -A 5 "definitely lost"

# Now apply fix and rebuild
cd /tmp/box64
git apply /path/to/patches/003_fix_mmaplist_chunks_leak.patch
cd build && make -j$(nproc) && sudo make install

# Run under valgrind again -> leak should be gone
valgrind --leak-check=full --show-leak-kinds=definite \
  box64 ./003_mmaplist_chunks_leak 2>&1 | grep -A 5 "definitely lost"
```

## CI Workflow — How Before/After Comparison Works

The GitHub Actions workflow (`leak-test.yml`) automates the comparison on an ARM64
runner. Here's exactly what it does:

```
┌─────────────────────────────────────────────────────────┐
│  1. Cross-compile test binary (x86_64)                  │
│     gcc-x86-64-linux-gnu → bin/003_mmaplist_chunks_leak │
└────────────────────────┬────────────────────────────────┘
                         │
         ┌───────────────┴───────────────┐
         ▼                               │
┌────────────────────┐                   │
│  2. BEFORE FIX     │                   │
│                    │                   │
│  Clone upstream    │                   │
│  ptitSeb/box64     │                   │
│  (main branch)     │                   │
│  Build & install   │                   │
│                    │                   │
│  Run under         │                   │
│  valgrind → log A  │                   │
└────────┬───────────┘                   │
         │                               │
         ▼                               │
┌────────────────────┐                   │
│  3. APPLY PATCH    │                   │
│                    │                   │
│  git apply patches/│                   │
│  003_fix_mmaplist_ │                   │
│  chunks_leak.patch │                   │
│  on the SAME clone │                   │
└────────┬───────────┘                   │
         │                               │
         ▼                               │
┌────────────────────┐                   │
│  4. AFTER FIX      │                   │
│                    │                   │
│  Rebuild box64     │                   │
│  (incremental,     │                   │
│   only custommem.c │                   │
│   recompiles)      │                   │
│                    │                   │
│  Run under         │                   │
│  valgrind → log B  │                   │
└────────┬───────────┘                   │
         │                               │
         ▼                               │
┌────────────────────────────────────────┐
│  5. COMPARE                            │
│                                        │
│  Extract "definitely lost" from both   │
│  logs and print side-by-side:          │
│                                        │
│  BEFORE: X bytes definitely lost       │
│  AFTER:  Y bytes definitely lost       │
│                                        │
│  Also grep for realloc/MmaplistAdd     │
│  in the leak stack traces to confirm   │
│  the specific chunks allocation.       │
└────────────────────────────────────────┘
```

**Key detail**: It uses the **same upstream clone** for both runs. The "BEFORE"
run uses unpatched `ptitSeb/box64 main`. Then `git apply` adds the fix, an
incremental `make` rebuilds only `custommem.o`, and the "AFTER" run uses the
patched binary. Same test binary, same valgrind flags, only difference is the fix.

**Why this works**: `box_realloc` maps to standard `realloc` (defined in
`src/include/debug.h`), so valgrind can track the `chunks` allocation. When
`free(head)` frees the struct without freeing `head->chunks`, valgrind reports it
as "definitely lost" because no pointer to that allocation remains reachable.

**Artifacts**: Both valgrind logs are uploaded as CI artifacts (30-day retention)
for manual inspection.
