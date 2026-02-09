# 003: mmaplist_t->chunks Pointer Array Leak

## Bug Summary

`mmaplist_t->chunks` is a dynamically allocated pointer array (via `box_realloc` in
`MmaplistAddBlock` and `MmaplistAddNBlocks`). It is never freed in two locations:

1. **`fini_custommem_helper()`** (shutdown): `free(head)` frees the `mmaplist_t`
   struct but not `head->chunks`. Additionally, `box_free(mmaplist)` at L3137 is
   dead code since `mmaplist` was set to `NULL` at L3130.

2. **`DelMmaplist()`** (runtime): `box_free(list)` frees the struct but not
   `list->chunks`. Called from `env.c` on every mapping removal (`dlclose`,
   `munmap`). **This is the high-impact path** — the leak accumulates over time.

## The Fix

Add `box_free(head->chunks)` / `box_free(list->chunks)` before freeing the struct.
Remove the dead `box_free(mmaplist)` line.

See: `patches/003_fix_mmaplist_chunks_leak.patch`

## Test Design — Two Phases

### Phase 1: Shutdown leak (small, one-time)

Hot inline loops create dynarec blocks in the **global** `mmaplist`. On program
exit, `fini_custommem_helper` leaks the global `chunks` array (~32 bytes).

### Phase 2: Runtime leak (scales linearly — the real problem)

Repeatedly `dlopen`/`dlclose` a shared library (`libhot.so`). Each cycle:

```
dlopen("libhot.so")
  → box64 creates a mapping_t with its own mmaplist_t (env.c:1450)

call hot_compute() / hot_compute_alt()
  → dynarec compiles blocks → MmaplistAddBlock grows chunks via box_realloc

dlclose(handle)
  → RemoveMapping (env.c:1374)
    → DelMmaplist (custommem.c:1547)
      → box_free(list)      ← frees the struct
      → list->chunks LEAKED ← never freed!
```

With 100 cycles, **100 separate `chunks` arrays leak** (~3200 bytes). With a
long-running application loading/unloading plugins or libraries, this grows
unboundedly.

## How to Verify with Valgrind

```bash
# Build the test binary and shared library (x86_64)
make CC=x86_64-linux-gnu-gcc

# Build box64 WITHOUT fix (from upstream main)
cd /tmp && git clone --depth 1 https://github.com/ptitSeb/box64.git
cd box64 && mkdir build && cd build
cmake .. -D ARM_DYNAREC=ON -D CMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc) && sudo make install

# Run 100 dlopen/dlclose cycles under valgrind
cd /path/to/bin
valgrind --leak-check=full --show-leak-kinds=definite \
  box64 ./003_mmaplist_chunks_leak 100 2>&1 | tee before.txt

# Apply fix and rebuild
cd /tmp/box64
git apply /path/to/patches/003_fix_mmaplist_chunks_leak.patch
cd build && make -j$(nproc) && sudo make install

# Run again
cd /path/to/bin
valgrind --leak-check=full --show-leak-kinds=definite \
  box64 ./003_mmaplist_chunks_leak 100 2>&1 | tee after.txt

# Compare
grep "definitely lost" before.txt after.txt
```

## CI Workflow — How Before/After Comparison Works

The GitHub Actions workflow (`leak-test.yml`) automates the comparison on an ARM64
runner. Here's exactly what it does:

```
┌─────────────────────────────────────────────────────────┐
│  1. Cross-compile test binary + libhot.so (x86_64)     │
│     gcc-x86_64-linux-gnu → bin/003_mmaplist_chunks_leak │
│                           → bin/libhot.so               │
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
│  cd bin && run     │                   │
│  under valgrind    │                   │
│  with 100 cycles   │                   │
│  → log A           │                   │
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
│  Incremental       │                   │
│  rebuild (only     │                   │
│  custommem.o)      │                   │
│                    │                   │
│  cd bin && run     │                   │
│  under valgrind    │                   │
│  with 100 cycles   │                   │
│  → log B           │                   │
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
│  The difference should be ~3200 bytes  │
│  (100 cycles × 32 bytes per chunks)    │
│  proving the leak scales with usage.   │
└────────────────────────────────────────┘
```

**Key detail**: It uses the **same upstream clone** for both runs. The "BEFORE"
run uses unpatched `ptitSeb/box64 main`. Then `git apply` adds the fix, an
incremental `make` rebuilds only `custommem.o`, and the "AFTER" run uses the
patched binary. Same test binary, same valgrind flags, only difference is the fix.

**Why this works**: `box_realloc` maps to standard `realloc` (defined in
`src/include/debug.h`), so valgrind can track the `chunks` allocation. When
`box_free(list)` frees the struct without freeing `list->chunks`, valgrind reports
it as "definitely lost" because no pointer to that allocation remains reachable.

**Artifacts**: Both valgrind logs are uploaded as CI artifacts (30-day retention)
for manual inspection.
