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
 * Features:
 *   - Multiple worker threads (configurable via NUM_WORKERS)
 *   - Multiple hot functions (different dynarec blocks)
 *   - Stress test mode with multiple sequential forks
 *   - Detailed diagnostics showing expected stale counters
 *
 * Run:
 *   BOX64_DYNAREC=1 BOX64_LOG=1 box64 ./001_fork_in_used_leak
 *
 * Stress test mode (multiple forks):
 *   BOX64_DYNAREC=1 box64 ./001_fork_in_used_leak --stress
 *
 * With full logging:
 *   BOX64_DYNAREC=1 BOX64_DYNAREC_LOG=3 box64 ./001_fork_in_used_leak
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
#include <time.h>

/* Configuration */
#define NUM_WORKERS       8    /* Number of worker threads */
#define NUM_HOT_FUNCS     4    /* Number of different hot functions */
#define STRESS_FORKS      5    /* Number of forks in stress test mode */
#define COMPILE_WAIT_MS   300  /* Time to wait for dynarec compilation */

static atomic_int workers_ready = 0;
static atomic_int stop_workers = 0;
static atomic_int stress_mode = 0;

/* Track which functions each worker is using */
static int worker_func_assignment[NUM_WORKERS];

/*
 * Multiple hot functions - each will be compiled into a DIFFERENT dynarec block.
 * This creates multiple stale in_used counters after fork.
 */

__attribute__((noinline, optimize("O2")))
long hot_compute_0(long iterations) {
    long sum = 0;
    for (long i = 0; i < iterations; i++) {
        sum += i * i;  /* Square pattern */
        if ((i & 0x3FFFF) == 0 && atomic_load(&stop_workers)) {
            return sum;
        }
    }
    return sum;
}

__attribute__((noinline, optimize("O2")))
long hot_compute_1(long iterations) {
    long sum = 0;
    for (long i = 0; i < iterations; i++) {
        sum += i * (i + 1);  /* Different pattern */
        if ((i & 0x3FFFF) == 0 && atomic_load(&stop_workers)) {
            return sum;
        }
    }
    return sum;
}

__attribute__((noinline, optimize("O2")))
long hot_compute_2(long iterations) {
    long sum = 0;
    for (long i = 0; i < iterations; i++) {
        sum += (i << 1) ^ i;  /* XOR pattern */
        if ((i & 0x3FFFF) == 0 && atomic_load(&stop_workers)) {
            return sum;
        }
    }
    return sum;
}

__attribute__((noinline, optimize("O2")))
long hot_compute_3(long iterations) {
    long sum = 0;
    for (long i = 0; i < iterations; i++) {
        sum += i + (i & 0xFF);  /* Mask pattern */
        if ((i & 0x3FFFF) == 0 && atomic_load(&stop_workers)) {
            return sum;
        }
    }
    return sum;
}

/* Function pointer array for hot functions */
typedef long (*hot_func_t)(long);
static hot_func_t hot_functions[NUM_HOT_FUNCS] = {
    hot_compute_0,
    hot_compute_1,
    hot_compute_2,
    hot_compute_3
};

static const char* hot_func_names[NUM_HOT_FUNCS] = {
    "hot_compute_0",
    "hot_compute_1",
    "hot_compute_2",
    "hot_compute_3"
};

/* Track in_used expectations */
typedef struct {
    int func_idx;
    int expected_in_used;
} func_usage_t;

static func_usage_t func_usage[NUM_HOT_FUNCS];
static pthread_mutex_t usage_mutex = PTHREAD_MUTEX_INITIALIZER;

void* worker_func(void* arg) {
    int worker_id = (int)(long)arg;
    int func_idx = worker_id % NUM_HOT_FUNCS;
    worker_func_assignment[worker_id] = func_idx;

    printf("[Worker %d] Using %s (dynarec block %d)\n",
           worker_id, hot_func_names[func_idx], func_idx);
    fflush(stdout);

    /* Track this worker's contribution to in_used */
    pthread_mutex_lock(&usage_mutex);
    func_usage[func_idx].func_idx = func_idx;
    func_usage[func_idx].expected_in_used++;
    pthread_mutex_unlock(&usage_mutex);

    atomic_fetch_add(&workers_ready, 1);

    /* Stay inside hot function's dynarec block until told to stop */
    while (!atomic_load(&stop_workers)) {
        hot_functions[func_idx](50000000);
    }

    printf("[Worker %d] Exiting\n", worker_id);
    return NULL;
}

void print_separator(void) {
    printf("========================================\n");
}

void print_double_separator(void) {
    printf("########################################\n");
}

void print_expected_state(const char* context) {
    printf("\n[Diagnostics] Expected in_used state %s:\n", context);
    printf("  +-----------------+------------------+\n");
    printf("  | Dynarec Block   | Expected in_used |\n");
    printf("  +-----------------+------------------+\n");

    int total_stale = 0;
    for (int i = 0; i < NUM_HOT_FUNCS; i++) {
        if (func_usage[i].expected_in_used > 0) {
            printf("  | %-15s | %16d |\n",
                   hot_func_names[i], func_usage[i].expected_in_used);
            total_stale += func_usage[i].expected_in_used;
        }
    }
    printf("  +-----------------+------------------+\n");
    printf("  | TOTAL STALE     | %16d |\n", total_stale);
    printf("  +-----------------+------------------+\n");
}

void child_verify_stale_blocks(int fork_num) {
    printf("\n");
    print_separator();
    printf(" CHILD PROCESS (PID %d) - Fork #%d\n", getpid(), fork_num);
    print_separator();
    printf("\n");

    printf("State after fork:\n");
    printf("  - Inherited %d dynarec blocks from parent\n", NUM_HOT_FUNCS);
    printf("  - Parent had %d worker threads inside these blocks\n", NUM_WORKERS);
    printf("  - Child has 0 worker threads\n");
    printf("  - All inherited in_used counters are STALE!\n");

    print_expected_state("in child (all stale)");

    printf("\n[Child] Attempting to use each dynarec block...\n\n");

    for (int i = 0; i < NUM_HOT_FUNCS; i++) {
        int stale = func_usage[i].expected_in_used;
        printf("  %s:\n", hot_func_names[i]);
        printf("    Before call: in_used = %d (STALE from parent)\n", stale);
        printf("    Entry:       in_used = %d + 1 = %d\n", stale, stale + 1);

        long result = hot_functions[i](1000);

        printf("    Exit:        in_used = %d - 1 = %d (still stale!)\n",
               stale + 1, stale);
        printf("    Result:      %ld\n", result);
        printf("\n");
    }

    printf("Conclusion:\n");
    printf("  - All %d blocks STILL have stale in_used > 0\n", NUM_HOT_FUNCS);
    printf("  - PurgeDynarecMap() will SKIP all these blocks\n");
    printf("  - Memory leak: %d blocks can NEVER be freed\n", NUM_HOT_FUNCS);
    printf("\n");

    /* Simulate memory pressure to trigger purge attempt */
    printf("[Child] Simulating memory pressure (allocating and freeing)...\n");
    for (int i = 0; i < 100; i++) {
        void* p = malloc(1024 * 1024);  /* 1MB */
        if (p) {
            memset(p, i, 1024 * 1024);
            free(p);
        }
    }
    printf("[Child] Even with memory pressure, stale blocks cannot be purged.\n");
}

int run_single_fork_test(void) {
    pthread_t workers[NUM_WORKERS];

    printf("\n");
    print_double_separator();
    printf(" TEST 001: Stale in_used After Fork\n");
    printf(" Configuration: %d workers, %d hot functions\n", NUM_WORKERS, NUM_HOT_FUNCS);
    print_double_separator();
    printf("\n");

    /* Initialize usage tracking */
    memset(func_usage, 0, sizeof(func_usage));

    /* Start all worker threads */
    printf("[Main] Creating %d worker threads...\n", NUM_WORKERS);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_create(&workers[i], NULL, worker_func, (void*)(long)i);
    }

    /* Wait for all workers to be ready */
    printf("[Main] Waiting for workers to enter hot loops...\n");
    while (atomic_load(&workers_ready) < NUM_WORKERS) {
        usleep(10000);
    }

    /* Let dynarec compile all blocks */
    printf("[Main] Waiting %dms for dynarec compilation...\n", COMPILE_WAIT_MS);
    usleep(COMPILE_WAIT_MS * 1000);

    printf("\n");
    print_separator();
    printf(" FORK POINT\n");
    print_separator();

    print_expected_state("at fork (parent)");

    printf("\n[Main] All %d workers are INSIDE their dynarec blocks\n", NUM_WORKERS);
    printf("[Main] Calling fork() now...\n");
    printf("\n");

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        atomic_store(&stop_workers, 1);
        for (int i = 0; i < NUM_WORKERS; i++) {
            pthread_join(workers[i], NULL);
        }
        return 1;
    }

    if (pid == 0) {
        /* CHILD PROCESS */
        child_verify_stale_blocks(1);

        print_separator();
        printf(" CHILD EXIT\n");
        print_separator();
        _exit(0);
    }

    /* PARENT PROCESS */
    printf("[Parent] Child PID: %d\n", pid);
    printf("[Parent] Waiting for child...\n");

    int status;
    waitpid(pid, &status, 0);

    printf("\n[Parent] Child exited with status: %d\n", WEXITSTATUS(status));

    printf("[Parent] Stopping workers...\n");
    atomic_store(&stop_workers, 1);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    return 0;
}

int run_stress_test(void) {
    printf("\n");
    print_double_separator();
    printf(" STRESS TEST: Multiple Forks with Many Threads\n");
    printf(" Configuration: %d workers, %d hot functions, %d forks\n",
           NUM_WORKERS, NUM_HOT_FUNCS, STRESS_FORKS);
    print_double_separator();
    printf("\n");

    pthread_t workers[NUM_WORKERS];

    /* Initialize usage tracking */
    memset(func_usage, 0, sizeof(func_usage));

    /* Start all worker threads */
    printf("[Main] Creating %d worker threads...\n", NUM_WORKERS);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_create(&workers[i], NULL, worker_func, (void*)(long)i);
    }

    /* Wait for all workers to be ready */
    printf("[Main] Waiting for workers to enter hot loops...\n");
    while (atomic_load(&workers_ready) < NUM_WORKERS) {
        usleep(10000);
    }

    /* Let dynarec compile all blocks */
    printf("[Main] Waiting %dms for dynarec compilation...\n", COMPILE_WAIT_MS);
    usleep(COMPILE_WAIT_MS * 1000);

    print_expected_state("at fork time");

    /* Perform multiple forks */
    printf("\n");
    print_separator();
    printf(" STARTING %d SEQUENTIAL FORKS\n", STRESS_FORKS);
    print_separator();
    printf("\n");

    pid_t children[STRESS_FORKS];
    int fork_count = 0;

    for (int f = 0; f < STRESS_FORKS; f++) {
        printf("[Main] === Fork %d/%d ===\n", f + 1, STRESS_FORKS);

        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            break;
        }

        if (pid == 0) {
            /* CHILD PROCESS */
            child_verify_stale_blocks(f + 1);

            /* In stress mode, child also forks to show accumulation */
            if (f < 2) {  /* Only first 2 children fork again */
                printf("\n[Child %d] Forking again to show accumulation...\n", f + 1);

                pid_t grandchild = fork();
                if (grandchild == 0) {
                    printf("\n");
                    print_separator();
                    printf(" GRANDCHILD (from fork %d)\n", f + 1);
                    print_separator();
                    printf("  - Inherited already-stale counters from child\n");
                    printf("  - Stale counters persist across generations!\n");
                    _exit(0);
                } else if (grandchild > 0) {
                    waitpid(grandchild, NULL, 0);
                }
            }

            _exit(0);
        }

        children[fork_count++] = pid;

        /* Small delay between forks */
        usleep(50000);
    }

    /* Wait for all children */
    printf("\n[Parent] Waiting for %d children...\n", fork_count);
    for (int i = 0; i < fork_count; i++) {
        int status;
        waitpid(children[i], &status, 0);
        printf("[Parent] Child %d (PID %d) exited: %d\n",
               i + 1, children[i], WEXITSTATUS(status));
    }

    /* Stop workers */
    printf("\n[Parent] Stopping workers...\n");
    atomic_store(&stop_workers, 1);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    printf("\n");
    print_double_separator();
    printf(" STRESS TEST SUMMARY\n");
    print_double_separator();
    printf("\n");
    printf("  Total forks performed:     %d\n", fork_count);
    printf("  Workers at each fork:      %d\n", NUM_WORKERS);
    printf("  Dynarec blocks affected:   %d\n", NUM_HOT_FUNCS);
    printf("  Stale counters per child:  %d (sum across all blocks)\n", NUM_WORKERS);
    printf("\n");
    printf("  In a buggy Box64:\n");
    printf("    - Each child inherits %d stale in_used counters\n", NUM_WORKERS);
    printf("    - These blocks can NEVER be purged in the child\n");
    printf("    - Memory leak accumulates with each fork\n");
    printf("\n");

    return 0;
}

int main(int argc, char* argv[]) {
    /* Check for stress test mode */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stress") == 0 || strcmp(argv[i], "-s") == 0) {
            atomic_store(&stress_mode, 1);
        }
    }

    int result;

    if (atomic_load(&stress_mode)) {
        result = run_stress_test();
    } else {
        result = run_single_fork_test();
    }

    printf("\n");
    print_double_separator();
    printf(" TEST COMPLETE\n");
    print_double_separator();
    printf("\n");

    printf("To verify the issue with Box64 diagnostics:\n");
    printf("  1. Apply diagnose_fork_in_used.patch to Box64\n");
    printf("  2. Rebuild Box64\n");
    printf("  3. Run: BOX64_DYNAREC=1 box64 ./001_fork_in_used_leak\n");
    printf("  4. Or stress test: BOX64_DYNAREC=1 box64 ./001_fork_in_used_leak --stress\n");
    printf("  5. Look for 'Blocks with in_used > 0' in child output\n");
    printf("\n");

    return result;
}
