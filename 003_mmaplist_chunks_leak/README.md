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

## CI Workflow

The GitHub Actions workflow (`leak-test.yml`) automates this before/after comparison
on an ARM64 runner with valgrind.
