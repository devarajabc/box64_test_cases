/*
 * 003_mmaplist_chunks_leak
 *
 * Test: Memory leak of mmaplist_t->chunks pointer array
 *
 * Issue:
 *   In fini_custommem_helper(), the code does:
 *     mmaplist_t* head = mmaplist;
 *     mmaplist = NULL;
 *     if(head) {
 *         for (int i=0; i<head->size; ++i)
 *             InternalMunmap(head->chunks[i]->block-sizeof(blocklist_t), ...);
 *         free(head);          // frees struct, NOT head->chunks
 *     }
 *     box_free(mmaplist);      // mmaplist is NULL -> no-op (dead code)
 *
 *   The head->chunks array (allocated via box_realloc in MmaplistAddBlock
 *   and MmaplistAddNBlocks) is never freed. Same bug exists in DelMmaplist().
 *
 * How to verify:
 *   valgrind --leak-check=full --show-leak-kinds=all \
 *     --suppressions=box64_known.supp \
 *     box64 ./003_mmaplist_chunks_leak
 *
 *   Before fix: valgrind reports "definitely lost" bytes from realloc()
 *   called in MmaplistAddBlock / MmaplistAddNBlocks.
 *
 *   After fix: That specific leak disappears.
 *
 * What this program does:
 *   Creates multiple hot loops so box64's DynaRec compiles them into
 *   native blocks, populating mmaplist->chunks. On normal exit,
 *   fini_custommem_helper runs and the leak (or fix) is exercised.
 */

#include <stdio.h>
#include <stdlib.h>

/* Prevent the compiler from optimizing these away */
volatile long sink = 0;

/*
 * Multiple distinct functions -> multiple dynarec blocks -> chunks array grows.
 * Using noinline to ensure each becomes a separate compilation unit for DynaRec.
 */
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

int main(void) {
    printf("=== 003: mmaplist_t->chunks leak test ===\n");
    printf("Creating dynarec blocks via hot loops...\n");

    /* Run enough iterations to ensure DynaRec compiles these blocks
     * and populates mmaplist->chunks via MmaplistAddBlock */
    for (int round = 0; round < 200; round++) {
        hot_loop_a(5000);
        hot_loop_b(5000);
        hot_loop_c(5000);
        hot_loop_d(5000);
    }

    printf("Done. sink=%ld\n", sink);
    printf("Normal exit -> fini_custommem_helper runs.\n");
    printf("\n");
    printf("If running under valgrind, check for:\n");
    printf("  'definitely lost' from realloc in MmaplistAddBlock\n");
    printf("  or MmaplistAddNBlocks (custommem.c)\n");

    return 0;
}
