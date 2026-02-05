# How Box64 Works: A Complete Step-by-Step Walkthrough part2

# Process & Thread Management

This section explains how box64 handles multi-threading, process forking, thread-local storage, and thread cancellation. We analyze four test programs (`test06`, `test09`, `test11`, `test14`) to trace the complete lifecycle of these operations through the emulator.

## Overview

Box64 runs x86_64 threads and processes on native host threads/processes. Each emulated thread gets its own `x64emu_t` CPU state, but all threads share the same `box64context_t` (global context, loaded libraries, bridge tables, dynarec blocks). Forking creates a full process copy via the OS `fork()` syscall, but the fork is **deferred** — it cannot happen inside a dynarec block because the generated ARM64 code holds state in ARM64 registers that must be saved first.

The key source files involved:

| File | Purpose |
|------|---------|
| `src/libtools/threads.c` | Thread creation, pthread wrappers, cancellation |
| `src/include/threads.h` | `emuthread_t` structure definition |
| `src/wrapped/wrappedlibc.c` | `fork()`/`vfork()` wrappers, `__register_atfork` |
| `src/emu/x64int3.c` | `x64emu_fork()` — actual fork execution |
| `src/emu/x64tls.c` | Thread-local storage setup and management |
| `src/dynarec/dynarec.c` | `EmuRun()` dynarec main loop with fork check |
| `src/emu/x64run.c` | `Run()` interpreter main loop with fork check |
| `src/os/os_linux.c` | `EmuFork()` OS-abstraction wrapper |
| `src/emu/x64emu_private.h` | Per-thread `x64emu_t` structure |
| `src/include/box64context.h` | Global context with atfork callbacks |

---

## 1. test06: Thread Creation & Join

### What the test does

`test06.c` creates 2 threads with `pthread_create()`, each running `doSomething()`. The main thread waits for both with `pthread_join()`:

```c
for (int i = 0; i < thread_count; ++i)
    err = pthread_create(&tid[i], NULL, doSomething, NULL);

for (int i = 0; i < thread_count; ++i)
    pthread_join(tid[i], NULL);
```

### Log output

```
[BOX64] Using native(wrapped) libpthread.so.0
[BOX64] Using native(wrapped) libc.so.6
...
[BOX64] set mapallmem: 0xffff59be0000, 0xffff59de0000, 0x9    ← thread 1 stack
[BOX64] set memprot: 0xffff59be0000, 0xffff59de0000, 0x3
[BOX64] set mapallmem: 0xffff58dd0000, 0xffff58fd0000, 0x9    ← thread 2 stack
[BOX64] set memprot: 0xffff58dd0000, 0xffff58fd0000, 0x3
[02] Second thread executing
[02] Thread done.

[00] Done.
[BOX64] freeProtection 0xffff59be0000:0xffff59ddffff           ← thread 1 stack freed
[BOX64] freeProtection 0xffff58dd0000:0xffff58fcffff           ← thread 2 stack freed
[BOX64] endBox64() called
```

### Step-by-step walkthrough

#### Step 1: `pthread_create()` → `my_pthread_create()` (`src/libtools/threads.c:580`)

When the emulated program calls `pthread_create()`, the call goes through a bridge to box64's wrapper `my_pthread_create()`. This function:

1. **Determines stack size** — defaults to 2 MB, or reads from the `pthread_attr_t` if provided:
   ```c
   int stacksize = 2*1024*1024;  // default 2 MB
   if(attr) {
       if(pthread_attr_getstacksize(PTHREAD_ATTR(attr), &stsize)==0)
           stacksize = stsize;
   }
   ```

2. **Allocates a stack** for the new thread using `InternalMmap()`:
   ```c
   stack = InternalMmap(NULL, stacksize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
   setProtection_stack((uintptr_t)stack, stacksize, PROT_READ|PROT_WRITE);
   ```
   This is what produces the `set mapallmem` log lines — box64 tracks all memory regions for protection and dynarec purposes.

3. **Creates a new `x64emu_t`** — the emulated CPU state for the new thread:
   ```c
   emuthread_t *et = (emuthread_t*)box_calloc(1, sizeof(emuthread_t));
   x64emu_t *emuthread = NewX64Emu(my_context, (uintptr_t)start_routine,
                                     (uintptr_t)stack, stacksize, own);
   SetupX64Emu(emuthread, emu);  // copies parent's segment registers, etc.
   et->emu = emuthread;
   et->fnc = (uintptr_t)start_routine;
   et->arg = arg;
   ```

4. **Pre-JITs the thread entry point** (if dynarec is enabled):
   ```c
   if(BOX64ENV(dynarec)) {
       DBGetBlock(emu, (uintptr_t)start_routine, 1, 0);
   }
   ```
   This pre-compiles the first dynarec block of the thread function so the new thread doesn't stall on compilation at startup.

5. **Calls native `pthread_create()`** with `pthread_routine` as the actual entry point:
   ```c
   return pthread_create((pthread_t*)t, PTHREAD_ATTR(attr), pthread_routine, et);
   ```

#### Step 2: `pthread_routine()` — new thread entry (`src/libtools/threads.c:293`)

The native OS creates a real thread that enters `pthread_routine()`. This function:

1. **Binds the `emuthread_t` to the thread** via `pthread_key`:
   ```c
   pthread_setspecific(thread_key, p);
   emuthread_t *et = (emuthread_t*)p;
   ```

2. **Initializes TLS** for this thread:
   ```c
   refreshTLSData(emu);
   ```

3. **Sets up the emulated call stack** — creates a proper x86_64 stack frame:
   ```c
   Push64(emu, 0);      // return address = 0 (backtrace marker)
   Push64(emu, 0);      // saved RBP = 0
   R_RBP = R_RSP;       // frame pointer
   R_RSP -= 64;         // guard zone
   R_RSP &= ~15LL;      // 16-byte alignment
   PushExit(emu);        // push exit bridge as return address
   R_RIP = et->fnc;     // set instruction pointer to thread function
   R_RDI = (uintptr_t)et->arg;  // first argument in RDI
   ```

4. **Registers a cancellation cleanup handler** and starts execution:
   ```c
   pthread_cleanup_push(emuthread_cancel, p);
   DynaRun(emu);
   pthread_cleanup_pop(0);
   void* ret = (void*)R_RAX;
   return ret;
   ```

   `DynaRun()` enters the same `EmuRun()` loop described in the Dynarec Deep Dive section — the thread function is JIT-compiled and executed as native ARM64 code. When the thread function returns, the `PushExit` bridge catches it, sets `emu->quit = 1`, and `DynaRun()` returns. The return value in RAX becomes the thread's return value.

#### Step 3: Thread cleanup

When the thread exits (or is joined), the `emuthread_destroy()` function (`src/libtools/threads.c:180`) is called via the `pthread_key` destructor:

```c
void emuthread_destroy(void* p) {
    emuthread_t *et = (emuthread_t*)p;
    if(!et) return;
    // (BOX32: cleanup 32-bit thread hash if needed)
    FreeX64Emu(&et->emu);  // free the per-thread CPU state
    // (BAD_PKILL: remove from thread tracking)
    box_free(et);           // free the emuthread_t wrapper
}
```

The full implementation includes conditional cleanup for BOX32 (32-bit support) and BAD_PKILL (platforms with broken `pkill`, like LoongArch). The thread stack memory is freed, producing the `freeProtection` log lines.

### The `emuthread_t` structure

Each thread wraps its emulation state in this structure (`src/include/threads.h:8`):

```c
typedef struct emuthread_s {
    uintptr_t   fnc;                // x86_64 thread function address
    void*       arg;                // thread function argument
    x64emu_t*   emu;                // per-thread CPU state
    int         join;               // join state tracking
    int         is32bits;           // BOX32 32-bit mode flag
    uintptr_t   self;               // pthread_self() value
    ulong_t     hself;              // hash of self (for BOX32)
    int         cancel_cap;         // cancellation buffer capacity
    int         cancel_size;        // active cancellation handlers count
    void**      cancels;            // stack of cancellation cleanup buffers
} emuthread_t;
```

### Thread lifecycle diagram

```
Main thread                          New thread (OS-level)
───────────                          ─────────────────────
pthread_create() wrapper
  │
  ├─ NewX64Emu()                     pthread_routine() starts
  ├─ SetupX64Emu()                     │
  ├─ DBGetBlock() [pre-JIT]            ├─ pthread_setspecific(thread_key, et)
  ├─ native pthread_create() ─────→    ├─ refreshTLSData()
  │                                    ├─ Setup stack frame (Push64, PushExit)
  │                                    ├─ R_RIP = thread function
  │                                    ├─ DynaRun(emu)
  │                                    │   └─ EmuRun() loop
  │                                    │       ├─ DBGetBlock() [compile]
  │                                    │       └─ native_prolog() [execute]
  │                                    │
pthread_join() ◄──── thread returns ◄──┤─ return R_RAX
  │                                    └─ emuthread_destroy()
  ▼                                        ├─ FreeX64Emu()
continues                                  └─ box_free(et)
```

---

## 2. test09: Process Forking

### What the test does

`test09.c` calls `fork()` to create a child process. The child increments `x` and prints; the parent decrements `x` and prints:

```c
void forkexample() {
    int x = 1;
    if (fork() == 0)
        printf("Child has x = %d\n", ++x);
    else {
        wait(NULL);
        printf("Parent has x = %d\n", --x);
    }
}
```

### Log output

```
[BOX64] endBox64() called                    ← child cleanup
...
Child has x = 2
[BOX64] endBox64() called                    ← parent cleanup
...
Parent has x = 0
```

### Step-by-step walkthrough

#### Step 1: The deferred fork pattern

When the emulated program calls `fork()`, the call reaches `my_fork()` in `src/wrapped/wrappedlibc.c:580`:

```c
pid_t EXPORT my_fork(x64emu_t* emu)
{
    emu->quit = 1;
    emu->fork = 1;  // use regular fork
    return 0;
}
```

This does **not** call `fork()` immediately. Instead, it sets two flags:
- `emu->quit = 1` — tells `EmuRun()` to exit the current execution loop
- `emu->fork = 1` — tells `EmuRun()` to fork before resuming

**Why defer?** During dynarec execution, the x86_64 register state lives in ARM64 hardware registers (x10-x25). A `fork()` inside a wrapper function would duplicate the process, but the child's `EmuRun()` would resume with stale register values because the prolog/epilog hasn't had a chance to store them back to the `x64emu_t` structure. By setting `quit`, the dynarec block finishes its current instruction, the **epilog** stores all registers back to `x64emu_t`, and control returns to `EmuRun()`.

#### Step 2: Fork check in the execution loops

After the dynarec block containing the `fork()` call completes, the epilog stores registers, and the execution loop checks the fork flag. This check exists in **both** execution paths:

**Dynarec path** — `EmuRun()` in `src/dynarec/dynarec.c:230`:
```c
if(emu->fork) {
    int forktype = emu->fork;
    emu->quit = 0;    // reset quit — we want to continue after forking
    emu->fork = 0;    // clear fork flag
    emu = EmuFork(emu, forktype);
}
```

**Interpreter path** — `Run()` in `src/emu/x64run.c:2445`:
```c
if(emu->fork) {
    addr = R_RIP;
    int forktype = emu->fork;
    emu->quit = 0;
    emu->fork = 0;
    emu = EmuFork(emu, forktype);
    if(step)
        return 0;
    goto x64emurun;
}
```

Both paths call `EmuFork()`, a thin OS-abstraction wrapper in `src/os/os_linux.c:52` (declared in `src/include/os.h:56`) that simply calls `x64emu_fork()`:

```c
void* EmuFork(void* emu, int forktype)
{
    return x64emu_fork((x64emu_t*)emu, forktype);
}
```

This means the deferred fork pattern works identically whether box64 is running with dynarec enabled or in interpreter-only mode (`BOX64_DYNAREC=0`).

#### Step 3: `x64emu_fork()` — the actual fork (`src/emu/x64int3.c:40`)

`x64emu_fork()` performs the fork in three phases:

**Phase 1 — Run atfork prepare handlers** (reverse order):
```c
for (int i=my_context->atfork_sz-1; i>=0; --i)
    if(my_context->atforks[i].prepare)
        EmuCall(emu, my_context->atforks[i].prepare);
```

**Phase 2 — Call the OS `fork()`:**
```c
if(forktype==2)
    v = forkpty(...);   // forkpty variant
else
    v = fork();         // regular fork (forktype 1 or 3)
```

At this point, the OS creates a full copy of the process. Both parent and child have identical copies of the `x64emu_t` state, all loaded libraries, the dynarec block cache, and all memory mappings.

**Phase 3 — Run atfork parent/child handlers:**
```c
if(v != 0) {
    // Parent: run parent handlers
    for (int i=0; i<my_context->atfork_sz; --i)   // ⚠ see note below
        if(my_context->atforks[i].parent)
            EmuCall(emu, my_context->atforks[i].parent);
    if(forktype==3)
        waitpid(v, NULL, WEXITED);  // vfork: parent waits for child
} else if(v==0) {
    // Child: run child handlers
    for (int i=0; i<my_context->atfork_sz; --i)   // ⚠ see note below
        if(my_context->atforks[i].child)
            EmuCall(emu, my_context->atforks[i].child);
}
R_EAX = v;  // set return value: 0 in child, child PID in parent
```

> **Note — likely bug in the source (`src/emu/x64int3.c:62,71`):** The parent and child atfork loops both use `--i` as the loop increment, but the loop variable starts at `i=0` and the condition is `i < atfork_sz`. On the first iteration `i` is 0, so handler index 0 is called; then `--i` makes `i` equal to `-1`, which is still less than `atfork_sz` (assuming any handlers are registered), so the loop continues with an out-of-bounds negative index. In practice this is likely masked because most programs register zero atfork handlers through the emulator, so `atfork_sz` is 0 and the loop body never executes. Compare with the *prepare* handler loop (line 43), which correctly uses `--i` with reverse iteration (`for (int i=atfork_sz-1; i>=0; --i)`). The parent/child loops should presumably use `++i` to iterate forward, matching POSIX `pthread_atfork` semantics (prepare runs in reverse order; parent and child run in registration order).

**Phase 4 — Resume execution:**

`x64emu_fork()` returns the `emu` pointer, and `EmuRun()` continues the `while(!emu->quit)` loop. In both parent and child, execution resumes at the instruction **after** the `fork()` call. The only difference is `R_EAX`: 0 in the child, the child's PID in the parent.

#### The `atfork_fnc_t` structure

Programs can register fork callbacks via `pthread_atfork()`, which box64 wraps as `my___register_atfork()` (`src/wrapped/wrappedlibc.c:2814`):

```c
typedef struct atfork_fnc_s {
    uintptr_t prepare;   // called in parent before fork (reverse order)
    uintptr_t parent;    // called in parent after fork
    uintptr_t child;     // called in child after fork
    void*     handle;    // library handle for cleanup
} atfork_fnc_t;
```

These callbacks are stored in `my_context->atforks[]` and invoked by `x64emu_fork()` via `EmuCall()` — which runs the x86_64 function through the emulator.

### Fork types

| `emu->fork` value | Meaning | Behavior |
|-------------------|---------|----------|
| 1 | Regular `fork()` | Standard process duplication |
| 2 | `forkpty()` | Fork with pseudo-terminal allocation |
| 3 | `vfork()` (simulated) | Calls regular `fork()`, not actual `vfork()` — the real `vfork()` call is commented out in `x64int3.c:52-53`. The parent then calls `waitpid(child, NULL, WEXITED)` to block until the child exits or calls `exec()`, simulating `vfork()` semantics. |

### Fork lifecycle diagram

```
EmuRun() loop
  │
  ├─ native_prolog() → dynarec block
  │     └─ ... → fork() wrapper called
  │           └─ my_fork(): emu->quit=1, emu->fork=1
  │     └─ epilog: store regs to x64emu_t
  │
  ├─ check: emu->fork != 0
  │     └─ x64emu_fork(emu, forktype=1)
  │           ├─ run atfork prepare handlers
  │           ├─ fork()  ──────────────────────────────┐
  │           │                                        │
  │           ├─ [Parent: v = child_pid]               ├─ [Child: v = 0]
  │           │   ├─ run atfork parent handlers        │   ├─ run atfork child handlers
  │           │   └─ R_EAX = child_pid                 │   └─ R_EAX = 0
  │           │                                        │
  │           └─ return emu                             └─ return emu
  │
  ├─ emu->quit=0, emu->fork=0
  └─ continue EmuRun() loop                            └─ continue EmuRun() loop
       (parent resumes x86_64 code)                         (child resumes x86_64 code)
```

---

## 3. test11: Thread-Local Storage (TLS)

### What the test does

`test11.c` uses the `__thread` keyword to declare per-thread variables and verifies that each thread has its own independent copy:

```c
__thread int TLS_data1 = 10;
__thread int TLS_data2 = 20;
__thread char TLS_data3[10];

void *thread_run(void *parm) {
    threadparm_t *gData = (threadparm_t *)parm;
    TLS_data1 = gData->data1;    // each thread writes different values
    TLS_data2 = gData->data2;
    // ...
}
```

### Log output

```
Create/start 2 threads
[BOX64] set mapallmem: 0xffff906a0000, 0xffff908a0000, 0x9    ← thread 1 stack
[BOX64] set mapallmem: 0xffff8f890000, 0xffff8fa90000, 0x9    ← thread 2 stack
Thread 1: Entered (10/20)
Thread 1: foo(), TLS data=0 2 "-1-"
Thread 2: Entered (10/20)
Thread 2: foo(), TLS data=1 4 "-2-"
Thread 2: bar(), TLS data=1 4 "-2-"
Thread 1: bar(), TLS data=0 2 "-1-"
Main completed
```

### Step-by-step walkthrough

#### Step 1: How `__thread` variables work in x86_64

On x86_64, `__thread` variables are stored in the Thread-Local Storage (TLS) segment, accessed via the `FS` segment register. When the compiler generates code like:

```c
TLS_data1 = gData->data1;
```

It becomes something like:

```asm
mov %eax, %fs:offset_of_TLS_data1
```

The `FS` segment register points to a per-thread memory area. Each thread has its own FS base, so the same `fs:offset` resolves to different physical addresses in different threads.

#### Step 2: TLS initialization per thread — `refreshTLSData()` (`src/emu/x64tls.c:372`)

When a new thread starts in `pthread_routine()`, one of the first things it does is call `refreshTLSData(emu)`. This function:

1. **Checks if TLS data exists** for this emulator instance. If not, allocates it:
   ```c
   if ((ptr = emu->tlsdata) == NULL) {
       ptr = (tlsdatasize_t*)fillTLSData(emu->context);
   }
   ```

2. **Resizes if needed** (when new libraries with TLS have been loaded since the last allocation):
   ```c
   if(ptr->tlssize != emu->context->tlssize)
       ptr = (tlsdatasize_t*)resizeTLSData(emu->context, ptr);
   ```

3. **Updates the FS segment offset** so `fs:` accesses resolve correctly:
   ```c
   emu->segs_offs[_FS] = new_offs;
   ```

#### Step 3: TLS data layout — `setupTLSData()` (`src/emu/x64tls.c:261`)

Each thread's TLS block is allocated and initialized by `setupTLSData()`. The TLS data size is **64K-aligned** (`(s+0xffff)&~0xffff`), and there is a fixed-size gap between TLS data and DTS entries (`POS_TLS = 0x200` / 512 bytes for 64-bit, `POS_TLS_32 = 0x50` / 80 bytes for 32-bit). DTS entries are populated **per loaded ELF** that has TLS sections. The layout below shows the 64-bit path (the 32-bit BOX32 path uses smaller pointers and different canary offset at `0x14`):

```
┌─────────────────────────────────────────────────────┐
│  ELF TLS data (copied from .tdata sections)         │  ← offset -tlssize to 0
│  (contains initial values of __thread variables)    │  (64K-aligned allocation)
├─────────────────────────────────────────────────────┤
│  Thread Control Block (TCB) at offset 0:            │  ← FS points here
│    0x00: self pointer (tcb)                         │
│    0x08: DTV pointer (dynamic thread vector)        │
│    0x10: self pointer (again, for glibc compat)     │
│    0x20: vsyscall address                           │
│    0x28: stack canary                               │
│    ... (POS_TLS = 0x200 bytes reserved)             │
├─────────────────────────────────────────────────────┤
│  Dynamic Thread Storage (DTS) entries               │
│    Per-ELF: [pointer to TLS data, module index]     │
│    (each entry is 16 bytes for 64-bit)              │
└─────────────────────────────────────────────────────┘
```

The `__thread` variables live at negative offsets from the FS base. When the x86_64 code does `mov %fs:-0x10, %eax`, box64's dynarec or interpreter:
1. Reads `emu->segs_offs[_FS]` to get the thread's TLS base
2. Adds the negative offset
3. Reads/writes the memory at that address

Since each thread has its own `x64emu_t` with its own `segs_offs[_FS]` pointing to its own TLS allocation, the `__thread` variables are properly isolated.

#### Step 4: How threads see initial TLS values

When thread 1 starts and prints `Entered (10/20)`, it's reading the initial values of `TLS_data1` and `TLS_data2`. These initial values come from the `.tdata` section of the ELF binary, which was copied into each thread's TLS block during `setupTLSData()`:

```c
memcpy((void*)((uintptr_t)ptr-context->tlssize), context->tlsdata, context->tlssize);
```

This is why both threads initially see `TLS_data1=10` and `TLS_data2=20` — those are the compile-time initializers from the source code.

After each thread assigns its own values (`TLS_data1 = gData->data1`), the values diverge. Thread 1 sees `0, 2, "-1-"` and thread 2 sees `1, 4, "-2-"` — each in its own independent TLS copy.

#### Step 5: Mutex and condition variable wrapping

test11 uses `pthread_mutex_lock/unlock` and `pthread_cond_wait/broadcast` to synchronize thread execution order. These are all wrapped by box64 using the native `libpthread` — the mutex and condition variable structures are the same native structures, so box64 passes them through with minimal translation (only alignment adjustment on some platforms via `PTHREAD_ATTR_ALIGN`).

### TLS allocation diagram

```
Thread 1's x64emu_t:                 Thread 2's x64emu_t:
┌──────────────────┐                 ┌──────────────────┐
│ segs_offs[_FS] ──┼──┐              │ segs_offs[_FS] ──┼──┐
│ tlsdata ─────────┼──┼──┐           │ tlsdata ─────────┼──┼──┐
└──────────────────┘  │  │           └──────────────────┘  │  │
                      │  │                                 │  │
           ┌──────────┘  │                      ┌──────────┘  │
           ▼             │                      ▼             │
  Thread 1's TLS block   │             Thread 2's TLS block   │
  ┌──────────────────┐   │             ┌──────────────────┐   │
  │ TLS_data1 = 0    │   │             │ TLS_data1 = 1    │   │
  │ TLS_data2 = 2    │   │             │ TLS_data2 = 4    │   │
  │ TLS_data3 = "-1-"│   │             │ TLS_data3 = "-2-"│   │
  ├──────────────────┤◄──┘             ├──────────────────┤◄──┘
  │ TCB (self, DTV)  │                 │ TCB (self, DTV)  │
  │ canary           │                 │ canary           │
  └──────────────────┘                 └──────────────────┘
```

---

## 4. test14: Thread Cancellation

### What the test does

`test14.c` creates a thread that sleeps indefinitely, then cancels it with `pthread_cancel()`. The thread has a cleanup handler registered via `pthread_cleanup_push()` that must execute during cancellation:

```c
static void* thread_main(void* args) {
    pthread_cleanup_push(&thread_cleanup, NULL);  // register cleanup
    thread_f();                                    // sleep forever
    pthread_cleanup_pop(0);                        // never reached
    return NULL;
}

// In main:
pthread_cancel(thread);  // cancel the sleeping thread
```

### Log output

```
[BOX64] set mapallmem: 0xffff11f00000, 0xffff12700000, 0x9    ← thread stack
Thread: thread_state = 1.
Main thread: thread_state == 1.
[BOX64] freeProtection 0xffff11f00000:0xffff126fffff           ← thread cleaned up
[BOX64] endBox64() called
Thread: thread_state = 2.                                      ← cleanup handler ran
Main thread: thread_state == 2.
Finished with no errors.
```

### Step-by-step walkthrough

#### Step 1: Cleanup handler registration — `my___pthread_register_cancel()` (`src/libtools/threads.c:657`)

When the x86_64 code calls `pthread_cleanup_push()`, glibc internally calls `__pthread_register_cancel()`. Box64 wraps this:

```c
EXPORT void my___pthread_register_cancel(x64emu_t* emu, x64_unwind_buff_t* buff) {
    emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
    if(et->cancel_cap == et->cancel_size) {
        et->cancel_cap += 8;
        et->cancels = box_realloc(et->cancels, sizeof(void*)*et->cancel_cap);
    }
    et->cancels[et->cancel_size++] = buff;
}
```

The `x64_unwind_buff_t` contains a `jmp_buf` that glibc's `pthread_cleanup_push` macro saved. This captures the x86_64 CPU state (registers, stack pointer) at the point where the cleanup handler was registered.

The `cancels` array on the `emuthread_t` maintains a stack of active cleanup handlers — `push` adds to the top, `pop` removes from the top. This mirrors glibc's `__pthread_cleanup_push`/`__pthread_cleanup_pop` behavior.

#### Step 2: `pthread_cancel()` triggers cancellation

When the main thread calls `pthread_cancel(thread)`, this goes through to the native `pthread_cancel()`. The OS delivers a cancellation signal to the target thread, which interrupts its `sleep()` call.

#### Step 3: Cleanup execution — `emuthread_cancel()` (`src/libtools/threads.c:196`)

Remember that `pthread_routine()` registered `emuthread_cancel` as a native cleanup handler:

```c
pthread_cleanup_push(emuthread_cancel, p);
DynaRun(emu);
pthread_cleanup_pop(0);
```

When cancellation occurs, the native pthread library calls `emuthread_cancel()`:

```c
static void emuthread_cancel(void* p) {
    emuthread_t *et = (emuthread_t*)p;
    // Walk cleanup handlers in LIFO order (last registered = first executed)
    for(int i=et->cancel_size-1; i>=0; --i) {
        et->emu->flags.quitonlongjmp = 0;
        et->emu->quit = 0;
        my_longjmp(et->emu, ((x64_unwind_buff_t*)et->cancels[i])->__cancel_jmp_buf, 1);
        DynaRun(et->emu);  // resume execution at the cleanup point
    }
    box_free(et->cancels);
    et->cancels = NULL;
    et->cancel_size = et->cancel_cap = 0;
}
```

For each registered cleanup handler:
1. `my_longjmp()` restores the x86_64 CPU state from the saved `jmp_buf` — this effectively "jumps back" to the point where `pthread_cleanup_push` was called
2. `DynaRun()` resumes execution, which runs the cleanup handler function (`thread_cleanup` in this case)
3. The cleanup handler calls `my___pthread_unwind_next()`, which sets `emu->quit = 1` to exit `DynaRun()`
4. The loop continues to the next cleanup handler (if any)

In test14, the cleanup handler `thread_cleanup()` sets `thread_state = 2` and broadcasts the condition variable, allowing the main thread to detect that cancellation completed successfully.

#### Step 4: `__pthread_unwind_next()` — signal end of cleanup (`src/libtools/threads.c:679`)

```c
EXPORT void my___pthread_unwind_next(x64emu_t* emu, x64_unwind_buff_t* buff) {
    emu->quit = 1;
}
```

This is the simplest function in the cancellation chain — it just tells `DynaRun()` to stop, so control returns to the `emuthread_cancel()` loop.

### Cancellation lifecycle diagram

```
Main thread                     Worker thread
───────────                     ─────────────
                                pthread_routine()
                                  ├─ DynaRun() starts
                                  │   ├─ pthread_cleanup_push()
                                  │   │   └─ my___pthread_register_cancel()
                                  │   │       └─ et->cancels[0] = jmp_buf
                                  │   ├─ thread_f()
                                  │   │   └─ sleep(1000) [blocks here]
                                  │   │
pthread_cancel(thread) ──────────►│   │   [cancellation signal delivered]
                                  │   │   └─ sleep() interrupted
                                  │   │
                                  │   └─ native cleanup runs:
                                  │       emuthread_cancel()
                                  │         ├─ my_longjmp(jmp_buf[0])
                                  │         ├─ DynaRun()
                                  │         │   └─ thread_cleanup() runs
                                  │         │       └─ thread_state = 2
                                  │         │       └─ __pthread_unwind_next()
                                  │         │           └─ emu->quit = 1
                                  │         │   └─ DynaRun() returns
                                  │         └─ free cancels
                                  │
                                  └─ emuthread_destroy()
                                      └─ FreeX64Emu()
```

---

## 5. Key Data Structures Summary

### Per-thread state: `x64emu_t` (relevant fields)

```c
typedef struct x64emu_s {
    reg64_t     regs[16];          // x86_64 GP registers
    x64flags_t  eflags;            // flags register
    reg64_t     ip;                // instruction pointer (RIP)
    uint16_t    segs[6];           // segment selectors (CS, DS, ES, FS, GS, SS)
    uintptr_t   segs_offs[6];     // segment base addresses (FS → TLS)
    int         quit;              // exit signal for EmuRun()
    int         error;             // error flag
    int         fork;              // deferred fork request (0/1/2/3)
    int         exit;              // exit flag
    emu_flags_t flags;             // emulation flags (includes quitonlongjmp,
                                   //   used in emuthread_cancel())
    forkpty_t*  forkpty_info;      // info for forkpty() variant
    JUMPBUFF*   jmpbuf;            // jump buffer for signal handling
    void*       stack2free;        // thread stack to free on exit
    void*       init_stack;        // initial stack address
    uint32_t    size_stack;        // stack size
    tlsdatasize_t *tlsdata;       // per-thread TLS data block
    base_segment_t segldt[16];    // LDT segment descriptors
    base_segment_t seggdt[16];    // GDT segment descriptors
    int         type;              // EMUTYPE_MAIN etc.
    box64context_t *context;       // shared global context
} x64emu_t;
```

### Per-thread wrapper: `emuthread_t`

```c
typedef struct emuthread_s {
    uintptr_t   fnc;               // thread entry function (x86_64 address)
    void*       arg;               // argument to thread function
    x64emu_t*   emu;               // this thread's CPU state
    int         join;              // join tracking
    int         is32bits;          // 32-bit mode flag
    uintptr_t   self;              // pthread_self() value
    ulong_t     hself;             // hash of pthread_self
    int         cancel_cap;        // cancellation handler array capacity
    int         cancel_size;       // number of active cleanup handlers
    void**      cancels;           // stack of x64_unwind_buff_t pointers
} emuthread_t;
```

### Global context: `box64context_t` (thread/fork-related fields)

```c
typedef struct box64context_s {
    // Thread management
    kh_threadstack_t *stacksizes;          // custom stack sizes from pthread_attr_setstack
    pthread_mutex_t   mutex_thread;        // protects thread list operations
    pthread_mutex_t   mutex_tls;           // protects TLS allocation/resize

    // Fork callbacks
    atfork_fnc_t     *atforks;             // registered pthread_atfork handlers
    int               atfork_sz;           // number of registered handlers
    int               atfork_cap;          // array capacity

    // TLS
    void*             tlsdata;             // master TLS data template
    int32_t           tlssize;             // total TLS data size
} box64context_t;
```

---

## 6. Thread & Process Lifecycle Overview

```
                    ┌──────────────────────────────────┐
                    │         box64 process            │
                    │                                  │
                    │  ┌──────────────────────────────┐│
                    │  │    box64context_t (shared)   ││
                    │  │  ┌─ libraries, bridges ───┐  ││
                    │  │  │  dynarec metadata*     │  ││
                    │  │  │  atfork handlers       │  ││
                    │  │  │  TLS template          │  ││
                    │  │  └────────────────────────┘  ││
                    │  │  *actual jump tables are     ││
                    │  │   global statics in          ││
                    │  │   custommem.c, not in        ││
                    │  │   box64context_t             ││
                    │  └──────────────────────────────┘│
                    │                                  │
   ┌────────────────┼──────────────┬───────────────┐   │
   │                │              │               │   │
   ▼                ▼              ▼               ▼   │
┌──────┐      ┌──────┐      ┌──────┐        ┌──────┐   │
│Main  │      │Thread│      │Thread│        │Thread│   │
│thread│      │  1   │      │  2   │   ...  │  N   │   │
├──────┤      ├──────┤      ├──────┤        ├──────┤   │
│emu_t │      │emu_t │      │emu_t │        │emu_t │   │
│ regs │      │ regs │      │ regs │        │ regs │   │
│ RIP  │      │ RIP  │      │ RIP  │        │ RIP  │   │
│ FS→  │      │ FS→  │      │ FS→  │        │ FS→  │   │
│ TLS₀ │      │ TLS₁ │      │ TLS₂ │        │ TLSₙ │   │
│stack₀│      │stack₁│      │stack₂│        │stackₙ│   │
└──────┘      └──────┘      └──────┘        └──────┘   │
   │                                                   │
   │ fork()                                            │
   ▼                                                   │
┌──────────────────────────────────┐                   │
│   Child process (COW inherited)  │                   │
│   ┌────────────────────────────┐ │                   │
│   │box64context_t (inherited,  │ │
│   │  mutexes reinit)           │ │                   │
│   └────────────────────────────┘ │                   │
│   ┌──────┐                       │                   │
│   │emu_t │ (copy of parent)      │                   │
│   │R_EAX │ = 0 (child)           │                   │
│   └──────┘                       │                   │
└──────────────────────────────────┘                   │
                                                       │
└──────────────────────────────────────────────────────┘
```

Each OS thread runs its own execution loop independently — either `EmuRun()` (dynarec) or `Run()` (interpreter, when `BOX64_DYNAREC=0`). They share the `box64context_t` (with appropriate locking for thread-safe operations like TLS allocation and dynarec block compilation). The dynarec jump tables (`box64_jmptbl3`, `box64_jmptbl4`) are global static arrays in `src/custommem.c:43-55`, shared across threads but not members of `box64context_t`—the context only holds metadata like `max_db_size` and `db_sizes`. When `fork()` is called, Linux uses copy-on-write semantics: the child inherits the same `my_context` pointer (now in its own address space via COW), and dynarec blocks, jump tables, and all other data are inherited as-is rather than reconstructed. The only child-specific work is re-initializing mutexes (which may have been held at fork time)—see `atfork_child_box64context()` in `src/box64context.c:132-136` and `atfork_child_custommem()` in `src/custommem.c:2929-2933`. Both parent and child resume from the same point in the execution loop with different return values in `R_EAX`.
