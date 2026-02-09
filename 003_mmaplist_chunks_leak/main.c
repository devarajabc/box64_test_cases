/*
 * 003_mmaplist_chunks_leak
 *
 * Test: Memory leak of mmaplist_t->chunks pointer array
 *
 * Bug:
 *   mmaplist_t->chunks (allocated via box_realloc in MmaplistAddBlock/
 *   MmaplistAddNBlocks) is never freed in two locations:
 *
 *   1. fini_custommem_helper(): free(head) frees the struct, not head->chunks.
 *      box_free(mmaplist) is dead code (mmaplist set to NULL earlier).
 *
 *   2. DelMmaplist(): box_free(list) frees the struct, not list->chunks.
 *      Called from env.c on every mapping removal (dlclose, munmap).
 *
 * How this test demonstrates the impact:
 *
 *   Phase 1 (shutdown leak):
 *     Hot loops create dynarec blocks in the global mmaplist.
 *     On exit, fini_custommem_helper leaks the global chunks array.
 *     This is a one-time ~32 byte leak.
 *
 *   Phase 2 (runtime leak - the real problem):
 *     Repeatedly dlopen/dlclose a shared library. Each cycle:
 *       - dlopen creates a new mapping with its own mmaplist_t
 *       - Calling library functions triggers dynarec → allocates chunks
 *       - dlclose calls RemoveMapping → DelMmaplist → leaks chunks
 *     With N cycles, N chunks arrays leak. The leak scales linearly.
 *
 * Run:
 *   valgrind --leak-check=full box64 ./003_mmaplist_chunks_leak [cycles]
 *
 *   Default: 100 dlopen/dlclose cycles.
 *   Compare "definitely lost" before and after applying the fix patch.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

#define DEFAULT_CYCLES 100

volatile long sink = 0;

/* ── Phase 1: Global mmaplist leak (shutdown path) ──────────────── */

__attribute__((noinline))
void hot_loop_a(int n) {
    long s = 0;
    for (int i = 0; i < n; i++)
        s += (long)i * i;
    sink += s;
}

__attribute__((noinline))
void hot_loop_b(int n) {
    long s = 0;
    for (int i = 0; i < n; i++)
        s += (long)i * (i + 1);
    sink += s;
}

__attribute__((noinline))
void hot_loop_c(int n) {
    long s = 0;
    for (int i = 0; i < n; i++)
        s ^= (long)i << 3;
    sink += s;
}

__attribute__((noinline))
void hot_loop_d(int n) {
    long s = 0;
    for (int i = 0; i < n; i++)
        s += (long)(i & 0xFF) * (i >> 8);
    sink += s;
}

/* ── Phase 2: Per-mapping mmaplist leak (runtime path) ──────────── */

static int dlopen_dlclose_cycle(const char *lib_path) {
    void *handle = dlopen(lib_path, RTLD_NOW);
    if (!handle) {
        return -1;
    }

    /* Call library functions to trigger dynarec block creation
     * within the library's mapping → populates mapping->mmaplist->chunks */
    typedef int (*compute_fn)(int);

    compute_fn fn1 = (compute_fn)dlsym(handle, "hot_compute");
    compute_fn fn2 = (compute_fn)dlsym(handle, "hot_compute_alt");

    if (fn1) {
        for (int j = 0; j < 20; j++)
            sink += fn1(2000);
    }
    if (fn2) {
        for (int j = 0; j < 20; j++)
            sink += fn2(2000);
    }

    /* dlclose triggers RemoveMapping → DelMmaplist
     * BUG: chunks array leaks here every time */
    dlclose(handle);
    return 0;
}

int main(int argc, char *argv[]) {
    int num_cycles = DEFAULT_CYCLES;
    if (argc > 1)
        num_cycles = atoi(argv[1]);
    if (num_cycles < 1)
        num_cycles = 1;

    printf("=== 003: mmaplist_t->chunks leak test ===\n\n");

    /* ── Phase 1: Global mmaplist ── */
    printf("Phase 1: Creating global dynarec blocks...\n");
    for (int round = 0; round < 200; round++) {
        hot_loop_a(5000);
        hot_loop_b(5000);
        hot_loop_c(5000);
        hot_loop_d(5000);
    }
    printf("  Global dynarec blocks created.\n");
    printf("  On exit, fini_custommem_helper will leak global chunks (~32 bytes).\n\n");

    /* ── Phase 2: Per-mapping leak via dlopen/dlclose ── */
    printf("Phase 2: %d dlopen/dlclose cycles on libhot.so...\n", num_cycles);
    printf("  Each cycle: dlopen → call hot functions (dynarec) → dlclose\n");
    printf("  BUG: Each dlclose leaks the mapping's chunks array.\n\n");

    int success = 0;
    int fail = 0;

    for (int i = 0; i < num_cycles; i++) {
        if (dlopen_dlclose_cycle("./libhot.so") == 0)
            success++;
        else
            fail++;

        if ((i + 1) % 50 == 0)
            printf("  ... completed %d/%d cycles\n", i + 1, num_cycles);
    }

    printf("\n");
    printf("Results:\n");
    printf("  Successful cycles: %d\n", success);
    printf("  Failed cycles:     %d\n", fail);
    printf("  sink = %ld\n\n", sink);

    if (fail > 0 && success == 0) {
        printf("WARNING: All dlopen calls failed.\n");
        printf("  Make sure libhot.so is in the same directory as this binary.\n");
        printf("  The test still shows the global mmaplist leak (Phase 1).\n\n");
    }

    printf("Expected leak (without fix):\n");
    printf("  - Phase 1: ~32 bytes (1 global chunks array)\n");
    printf("  - Phase 2: ~32 bytes x %d cycles = ~%d bytes\n",
           success, success * 32);
    printf("  - Total:   ~%d bytes from chunks arrays alone\n\n",
           32 + success * 32);

    printf("After fix: All chunks arrays freed, these leaks disappear.\n");

    return 0;
}
