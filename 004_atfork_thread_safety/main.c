/*
 * 004_atfork_thread_safety
 *
 * Test: Race condition in pthread_atfork / __register_atfork registration
 *
 * Issue:
 *   In box64, both my_pthread_atfork (wrappedlibpthread.c) and
 *   my___register_atfork (wrappedlibc.c) modify the shared
 *   my_context->atforks array and atfork_sz/atfork_cap without any
 *   locking. When multiple threads register atfork handlers concurrently:
 *
 *   1. Two threads read atfork_sz == atfork_cap simultaneously
 *   2. Both call realloc() on the same pointer → double-free / corruption
 *   3. Two threads read the same atfork_sz, both write to the same slot
 *      → one handler is silently lost
 *   4. Non-atomic atfork_sz++ → counter can skip or duplicate
 *
 * Test approach:
 *   - N threads each register M atfork handlers concurrently
 *   - Each prepare/parent/child handler increments an atomic counter
 *   - After all registrations complete, fork()
 *   - Compare expected handler count (N*M) vs actual invocations
 *   - Repeat multiple rounds to increase race probability
 *
 * Expected behavior (correct):
 *   All N*M handlers should be registered and all should fire during fork.
 *
 * Buggy behavior:
 *   - Fewer handlers fire than expected (lost due to race)
 *   - Crash during registration (double realloc)
 *   - Crash during fork (corrupted function pointers)
 *
 * Run:
 *   box64 ./004_atfork_thread_safety
 *   box64 ./004_atfork_thread_safety --rounds 10
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdatomic.h>
#include <errno.h>

/* Configuration */
#define NUM_THREADS       8     /* Threads registering concurrently */
#define HANDLERS_PER_THREAD 16  /* Each thread registers this many */
#define DEFAULT_ROUNDS    5     /* Number of fork rounds */
#define MAX_HANDLERS      4096  /* Safety limit */

/* Atomic counters — incremented by atfork handlers */
static atomic_int prepare_count = 0;
static atomic_int parent_count = 0;
static atomic_int child_count = 0;

/* Synchronization: all threads start registering at the same time */
static pthread_barrier_t start_barrier;

/* Track registration results */
static atomic_int register_success = 0;
static atomic_int register_fail = 0;

/*
 * Atfork handler functions.
 * Each simply increments an atomic counter so we can verify
 * the correct number of handlers were called.
 */
static void prepare_handler(void)
{
    atomic_fetch_add(&prepare_count, 1);
}

static void parent_handler(void)
{
    atomic_fetch_add(&parent_count, 1);
}

static void child_handler(void)
{
    atomic_fetch_add(&child_count, 1);
}

/*
 * Worker thread: registers HANDLERS_PER_THREAD atfork handlers.
 * All threads wait at the barrier to maximize concurrent registration.
 */
static void *register_worker(void *arg)
{
    int thread_id = (int)(long)arg;
    int success = 0;
    int fail = 0;

    /* Wait for all threads to be ready */
    pthread_barrier_wait(&start_barrier);

    /* Register handlers as fast as possible to trigger the race */
    for (int i = 0; i < HANDLERS_PER_THREAD; i++) {
        int ret = pthread_atfork(prepare_handler, parent_handler, child_handler);
        if (ret == 0) {
            success++;
        } else {
            fail++;
            fprintf(stderr, "[Thread %d] pthread_atfork failed: %s (handler %d)\n",
                    thread_id, strerror(ret), i);
        }
    }

    atomic_fetch_add(&register_success, success);
    atomic_fetch_add(&register_fail, fail);

    return NULL;
}

/*
 * Run one round: concurrent registration + fork + verify counts.
 * Returns 0 on success, 1 on detected corruption.
 */
static int run_round(int round_num, int expected_total)
{
    int result = 0;

    /* Reset counters */
    atomic_store(&prepare_count, 0);
    atomic_store(&parent_count, 0);
    atomic_store(&child_count, 0);

    printf("[Round %d] Forking with %d registered handlers...\n",
           round_num, expected_total);
    fflush(stdout);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* CHILD PROCESS */
        int c_prepare = atomic_load(&prepare_count);
        int c_child   = atomic_load(&child_count);

        printf("[Round %d][Child PID %d] prepare=%d (expect %d), child=%d (expect %d)",
               round_num, getpid(), c_prepare, expected_total, c_child, expected_total);

        if (c_prepare != expected_total || c_child != expected_total) {
            printf(" ** MISMATCH **\n");
            _exit(1);
        } else {
            printf(" OK\n");
            _exit(0);
        }
    }

    /* PARENT PROCESS */
    int p_prepare = atomic_load(&prepare_count);
    int p_parent  = atomic_load(&parent_count);

    printf("[Round %d][Parent]        prepare=%d (expect %d), parent=%d (expect %d)",
           round_num, p_prepare, expected_total, p_parent, expected_total);

    if (p_prepare != expected_total || p_parent != expected_total) {
        printf(" ** MISMATCH **\n");
        result = 1;
    } else {
        printf(" OK\n");
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) != 0) {
            printf("[Round %d] Child detected mismatch (exit code %d)\n",
                   round_num, WEXITSTATUS(status));
            result = 1;
        }
    } else if (WIFSIGNALED(status)) {
        printf("[Round %d] Child CRASHED with signal %d (%s)\n",
               round_num, WTERMSIG(status), strsignal(WTERMSIG(status)));
        result = 1;
    }

    return result;
}

int main(int argc, char *argv[])
{
    int rounds = DEFAULT_ROUNDS;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--rounds") == 0 || strcmp(argv[i], "-r") == 0) && i + 1 < argc) {
            rounds = atoi(argv[++i]);
            if (rounds < 1) rounds = 1;
            if (rounds > 100) rounds = 100;
        }
    }

    int total_expected = NUM_THREADS * HANDLERS_PER_THREAD;

    printf("========================================\n");
    printf(" 004: atfork Thread Safety Test\n");
    printf("========================================\n");
    printf(" Threads:              %d\n", NUM_THREADS);
    printf(" Handlers per thread:  %d\n", HANDLERS_PER_THREAD);
    printf(" Total handlers:       %d\n", total_expected);
    printf(" Fork rounds:          %d\n", rounds);
    printf("========================================\n\n");

    if (total_expected > MAX_HANDLERS) {
        fprintf(stderr, "Error: too many handlers (%d > %d)\n", total_expected, MAX_HANDLERS);
        return 1;
    }

    /*
     * Phase 1: Concurrent registration
     *
     * All threads wait at a barrier then register handlers simultaneously.
     * This maximizes the chance of hitting the race in box64's
     * my_pthread_atfork / my___register_atfork.
     */
    printf("Phase 1: Concurrent handler registration\n");
    printf("-----------------------------------------\n");

    pthread_barrier_init(&start_barrier, NULL, NUM_THREADS);

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        int ret = pthread_create(&threads[i], NULL, register_worker, (void *)(long)i);
        if (ret != 0) {
            fprintf(stderr, "pthread_create failed: %s\n", strerror(ret));
            return 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&start_barrier);

    int total_success = atomic_load(&register_success);
    int total_fail = atomic_load(&register_fail);

    printf("Registration complete:\n");
    printf("  Successful: %d\n", total_success);
    printf("  Failed:     %d\n", total_fail);

    if (total_success != total_expected) {
        printf("  ** WARNING: expected %d successful, got %d **\n",
               total_expected, total_success);
    }
    printf("\n");

    /*
     * Phase 2: Fork rounds — verify handler execution counts
     *
     * Each fork should invoke exactly total_success prepare handlers,
     * total_success parent handlers (in parent), and total_success
     * child handlers (in child).
     *
     * Note: atfork handlers accumulate across the process lifetime
     * (they cannot be unregistered). Each round uses the same handlers.
     */
    printf("Phase 2: Fork verification (%d rounds)\n", rounds);
    printf("-----------------------------------------\n");

    int failures = 0;
    for (int r = 1; r <= rounds; r++) {
        int ret = run_round(r, total_success);
        if (ret != 0) {
            failures++;
        }
        fflush(stdout);
    }

    /*
     * Phase 3: Summary
     */
    printf("\n========================================\n");
    printf(" RESULTS\n");
    printf("========================================\n");
    printf(" Registered:  %d / %d handlers\n", total_success, total_expected);
    printf(" Fork rounds: %d\n", rounds);
    printf(" Failures:    %d\n", failures);

    if (failures > 0) {
        printf("\n FAIL: %d round(s) had handler count mismatches.\n", failures);
        printf("   This indicates a thread safety bug in atfork registration.\n");
        printf("   Possible causes:\n");
        printf("   - Race on atfork_sz (lost handlers)\n");
        printf("   - Race on realloc (corrupted array)\n");
        printf("   - Corrupted function pointers (wrong handler called)\n");
    } else if (total_success != total_expected) {
        printf("\n FAIL: Registration lost %d handlers.\n", total_expected - total_success);
    } else {
        printf("\n PASS: All handlers registered and invoked correctly.\n");
    }
    printf("========================================\n");

    return (failures > 0 || total_success != total_expected) ? 1 : 0;
}
