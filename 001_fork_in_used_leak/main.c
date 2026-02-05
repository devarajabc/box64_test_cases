/*
 * 001_fork_in_used_leak
 *
 * Test: Stale dynablock in_used counter after fork()
 *
 * Issue:
 *   When a multi-threaded process calls fork() while other threads are
 *   executing inside dynarec blocks, the child inherits the in_used
 *   counters from those threads. Since those threads don't exist in the
 *   child, the counters become permanently stale, preventing
 *   PurgeDynarecMap() from ever freeing those blocks.
 *
 * Expected behavior (with fix):
 *   Child's in_used counters should be reset to 0 after fork.
 *
 * Current behavior (bug):
 *   Child inherits stale in_used > 0, blocks can never be purged.
 *
 * Run:
 *   BOX64_DYNAREC=1 BOX64_LOG=1 box64 ./001_fork_in_used_leak
 *
 * With full logging:
 *   BOX64_DYNAREC=1 BOX64_DYNAREC_LOG=3 box64 ./001_fork_in_used_leak
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdatomic.h>

static atomic_int worker_ready = 0;
static atomic_int stop_worker = 0;

/*
 * Hot function - will be compiled into a dynarec block.
 * Worker thread stays inside this while main thread forks.
 */
__attribute__((noinline))
long hot_compute(long iterations) {
    long sum = 0;
    for (long i = 0; i < iterations; i++) {
        sum += i * i;
        if ((i & 0x3FFFF) == 0 && atomic_load(&stop_worker)) {
            return sum;
        }
    }
    return sum;
}

void* worker_func(void* arg) {
    (void)arg;
    printf("[Worker] Entering hot loop (dynarec will compile this)...\n");
    fflush(stdout);

    atomic_store(&worker_ready, 1);

    /* Stay inside hot_compute's dynarec block until told to stop */
    while (!atomic_load(&stop_worker)) {
        hot_compute(100000000);
    }

    printf("[Worker] Exiting\n");
    return NULL;
}

void print_separator(void) {
    printf("========================================\n");
}

int main(void) {
    pthread_t worker;

    printf("\n");
    print_separator();
    printf(" TEST 001: Stale in_used After Fork\n");
    print_separator();
    printf("\n");

    /* Start worker thread */
    printf("[Main] Creating worker thread...\n");
    pthread_create(&worker, NULL, worker_func, NULL);

    /* Wait for worker to enter hot loop */
    while (!atomic_load(&worker_ready)) {
        usleep(1000);
    }

    /* Let dynarec compile the block */
    printf("[Main] Waiting for dynarec compilation (200ms)...\n");
    usleep(200000);

    printf("\n");
    print_separator();
    printf(" FORK POINT\n");
    print_separator();
    printf("\n");
    printf("[Main] Worker is INSIDE hot_compute's dynarec block\n");
    printf("[Main] Block's in_used >= 1\n");
    printf("[Main] Calling fork() now...\n");
    printf("\n");

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        atomic_store(&stop_worker, 1);
        pthread_join(worker, NULL);
        return 1;
    }

    if (pid == 0) {
        /* CHILD PROCESS */
        printf("\n");
        print_separator();
        printf(" CHILD PROCESS (PID %d)\n", getpid());
        print_separator();
        printf("\n");

        printf("State after fork:\n");
        printf("  - Inherited hot_compute's dynarec block\n");
        printf("  - Block has in_used >= 1 (from parent's worker)\n");
        printf("  - Child has NO worker thread\n");
        printf("  - in_used is STALE - will never decrement!\n");
        printf("\n");

        printf("[Child] Calling hot_compute (same dynarec block)...\n");
        printf("  Before: in_used = STALE (e.g., 1)\n");
        printf("  Entry:  in_used = STALE + 1 = 2\n");
        long r = hot_compute(1000);
        printf("  Exit:   in_used = 2 - 1 = 1 (still STALE!)\n");
        printf("  Result: %ld\n", r);
        printf("\n");

        printf("Conclusion:\n");
        printf("  - Block STILL has in_used = 1 (not 0)\n");
        printf("  - PurgeDynarecMap() will SKIP this block\n");
        printf("  - Memory leak: block can NEVER be freed\n");
        printf("\n");

        print_separator();
        printf(" CHILD EXIT\n");
        print_separator();
        _exit(0);
    }

    /* PARENT PROCESS */
    printf("[Parent] Child PID: %d\n", pid);
    sleep(1);

    printf("[Parent] Stopping worker...\n");
    atomic_store(&stop_worker, 1);
    pthread_join(worker, NULL);

    int status;
    waitpid(pid, &status, 0);
    printf("[Parent] Child exited: %d\n", WEXITSTATUS(status));

    printf("\n");
    print_separator();
    printf(" TEST COMPLETE\n");
    print_separator();
    printf("\n");

    printf("To verify the issue with Box64 diagnostics:\n");
    printf("  1. Apply diagnose_fork_in_used.patch to Box64\n");
    printf("  2. Rebuild Box64\n");
    printf("  3. Run this test again\n");
    printf("  4. Look for 'Blocks with in_used > 0' in child\n");
    printf("\n");

    return 0;
}
