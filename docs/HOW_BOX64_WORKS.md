# How Box64 Works: A Complete Step-by-Step Walkthrough

This document traces **every step** of how box64 executes an x86_64 binary, using `tests/test01` as a concrete example.

## What is test01?

`tests/test01.c` is a minimal x86_64 program that prints "Hello x86_64 World!" using a raw `syscall` instruction:

```c
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char msg[] = "Hello x86_64 World!\n";
    asm (
        "mov $1, %%rax \n"    // syscall number 1 = sys_write
        "mov $1, %%rdi \n"    // fd = 1 (stdout)
        "mov %0, %%rsi \n"    // buf = msg
        "mov $20, %%rdx \n"   // count = 20
        "syscall \n"          // invoke kernel
    :
    :"r" (msg)
    :"%rax","%rdi","%rsi","%rdx"
    );
    return 0;
}
```

When compiled, the `main` function produces these x86_64 machine instructions:

```asm
0000000000001129 <main>:
    1129: push   %rbp
    112a: mov    %rsp,%rbp
    112d: mov    %edi,-0x24(%rbp)        ; save argc
    1130: mov    %rsi,-0x30(%rbp)        ; save argv
    1134: movabs $0x3878206f6c6c6548,%rax ; "Hello x8"
    113e: mov    %rax,-0x20(%rbp)
    1142: movabs $0x726f572034365f36,%rax ; "6_64 Wor"
    114c: mov    %rax,-0x18(%rbp)
    1150: movabs $0xa21646c726f57,%rax    ; "ld!\n\0"
    115a: mov    %rax,-0x13(%rbp)
    115e: lea    -0x20(%rbp),%rcx        ; rcx = &msg
    1162: mov    $0x1,%rax               ; syscall# = 1 (write)
    1169: mov    $0x1,%rdi               ; fd = 1 (stdout)
    1170: mov    %rcx,%rsi               ; buf = &msg
    1173: mov    $0x14,%rdx              ; len = 20
    117a: syscall                        ; <-- THE KEY INSTRUCTION
    117c: mov    $0x0,%eax               ; return 0
    1181: pop    %rbp
    1182: ret
```

The challenge: this is x86_64 machine code. Your ARM64 (aarch64) CPU cannot execute it directly. Box64 makes it work.

---

## The Big Picture

```
┌──────────────────────────────────────────────────────────────┐
│                     box64 (ARM64 binary)                     │
│                                                              │
│  ┌────────────┐  ┌──────────────┐   ┌──────────────────────┐ │
│  │   main()   │→ │ initialize() │→  │     emulate()        │ │
│  │  src/main.c│  │  src/core.c  │   │     src/core.c       │ │
│  └────────────┘  └──────┬───────┘   └──────────┬───────────┘ │
│                         │                      │             │
│            ┌────────────┴────────┐    ┌────────┴──────────┐  │
│            │  1. Parse ELF       │    │  DynaRun()        │  │
│            │  2. Load into memory│    │   ┌─────────────┐ │  │
│            │  3. Load libraries  │    │   │ Dynarec:    │ │  │
│            │  4. Relocate symbols│    │   │ x86→ARM64   │ │  │
│            │  5. Setup CPU state │    │   │ translation │ │  │
│            │  6. Setup stack     │    │   ├─────────────┤ │  │
│            └─────────────────────┘    │   │ Interpreter │ │  │
│                                       │   │ fallback    │ │  │
│                                       │   └─────────────┘ │  │
│                                       └───────────────────┘  │
│                                                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐    │
│  │  Wrapped Libraries  │  │   Syscall Handler           │    │
│  │  libc.so.6 (native) │  │   x86 syscall → ARM64 call  │    │
│  │  libpthread.so.0    │  └─────────────────────────────┘    │
│  └─────────────────────┘                                     │
└──────────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Execution

### STEP 1: Entry Point (`src/main.c`)

When you run `./build/box64 ./tests/test01`, the ARM64 box64 binary starts at its own `main()`:

```c
// src/main.c
int main(int argc, const char **argv, char **env) {
    x64emu_t* emu = NULL;
    elfheader_t* elf_header = NULL;
    if (initialize(argc, argv, env, &emu, &elf_header, 1)) {
        return -1;
    }
    return emulate(emu, elf_header);
}
```

Two functions do everything:
1. **`initialize()`** - Parse the ELF, load libraries, set up emulated CPU
2. **`emulate()`** - Run the x86_64 code

---

### STEP 2: Environment & System Setup (`src/core.c:718`)

`initialize()` begins by detecting the host system:

```
[BOX64] Box64 arm64 v0.4.1 c389bdff4 with Dynarec built on Feb  1 2026 21:30:51
[BOX64] Dynarec for ARM64, with extension: ASIMD AES CRC32 PMULL ATOMICS SHA1 SHA2 USCAT FLAGM FLAGM2 FRINT
[BOX64] Running on Icestorm-M1 with 8 cores, pagesize: 16384
[BOX64] Will use hardware counter measured at 24.0 MHz emulating 3.0 GHz
```

Key actions:
- **Get page size**: `box64_pagesize = sysconf(_SC_PAGESIZE)` → 16384 (16KB on Apple Silicon)
- **Load environment variables**: `LoadEnvVariables()` reads BOX64_LOG, BOX64_DYNAREC, etc.
- **Detect CPU features**: ARM64 NEON, AES, CRC32, atomics - used to pick optimal dynarec codegen
- **Setup RDTSC emulation**: x86 `RDTSC` instruction reads a timestamp counter; box64 maps this to the ARM64 hardware counter, scaling 24 MHz → 3 GHz to match what x86 programs expect

---

### STEP 3: ELF Header Parsing (`src/core.c:1205`)

Box64 opens `tests/test01` and validates the ELF header:

```c
elfheader_t *elf_header = LoadAndCheckElfHeader(f, my_context->fullpath, 1);
```

The ELF header of test01 tells box64:
```
  Magic:   7f 45 4c 46 02 01 01 00    (ELF, 64-bit, little-endian)
  Type:    DYN (Position-Independent Executable)
  Machine: Advanced Micro Devices X86-64
  Entry:   0x1040 (_start function)
```

Box64 parses:
```
[BOX64] Read 31 Section header
[BOX64] Read 13 Program header
[BOX64] Loading SymTab (idx = 28)
[BOX64] Loading Dynamic (idx = 22)
[BOX64] The DT_INIT is at address 0x1000
[BOX64] The DT_FINI is at address 0x1184
[BOX64] The .text is at address 0x1040, and is 323 big
```

The **13 program headers** describe memory segments:
| Type | VirtAddr | Flags | Purpose |
|------|----------|-------|---------|
| LOAD | 0x0000 | R | Read-only data (ELF headers, symbol tables) |
| LOAD | 0x1000 | R+E | **Executable code** (.text, .init, .plt) |
| LOAD | 0x2000 | R | Read-only data (.rodata, .eh_frame) |
| LOAD | 0x3df0 | R+W | Writable data (.got, .data, .bss) |

---

### STEP 4: Memory Allocation & Loading (`src/core.c:1259`)

Box64 loads the x86_64 binary into its own process memory:

```c
CalcLoadAddr(elf_header);      // Calculate where to put it
AllocLoadElfMemory(my_context, elf_header, 1);  // Allocate and copy
```

Since test01 is a PIE (Position-Independent Executable), box64 chooses a base address:

```
[BOX64] Pre-allocated 0x4018 byte at 0x100000000 for tests/test01
[BOX64] Delta of 0x100000000 (vaddr=0x0) for Elf "tests/test01"
[BOX64] Allocating 0x4000 bytes @0x100000000, will read 0x610 @0x100000000
[BOX64] Allocating 0x4000 bytes @0x100004000, will read 0x18d @0x100001000
[BOX64] Allocating 0x4000 bytes @0x100008000, will read 0x220 @0x100003df0
[BOX64] Mapping tests/test01 in 0x100000000-0x100004018
```

So the binary is loaded at base `0x100000000`:
- Code (.text) at `0x100001040` (entry = `_start`)
- main() at `0x100001129`
- The `syscall` instruction at `0x10000117a`

---

### STEP 5: Stack Allocation (`src/core.c:1365`)

Box64 allocates a stack for the emulated x86_64 program:

```
[BOX64] Calc stack size, based on 1 elf(s)
[BOX64] Stack is @0xffff032c0000 size=0x800000 align=0x10
```

That's an 8 MB stack (0x800000 = 8,388,608 bytes), same as the Linux default.

---

### STEP 6: CPU Emulator Creation (`src/core.c:1373`)

Box64 creates an emulated x86_64 CPU - the `x64emu_t` structure:

```c
x64emu_t *emu = NewX64Emu(my_context, my_context->ep, (uintptr_t)my_context->stack, my_context->stacksz, 0);
SetupInitialStack(emu);
SetupX64Emu(emu, NULL);
```

```
[BOX64] Allocate a new X86_64 Emu, with RIP=0x0 and Stack=0xffff032c0000/0x800000
[BOX64] Setup X86_64 Emu
```

The `x64emu_t` structure (`src/emu/x64emu_private.h`) contains the **entire x86_64 CPU state**:

```c
typedef struct x64emu_s {
    reg64_t     regs[16];       // RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8-R15
    x64flags_t  eflags;         // ZF, CF, OF, SF, PF, AF flags
    reg64_t     ip;             // RIP - instruction pointer

    sse_regs_t  xmm[16];       // XMM0-XMM15 (128-bit SIMD)
    sse_regs_t  ymm[16];       // YMM0-YMM15 (256-bit AVX)

    mmx87_regs_t x87[8];       // x87 FPU stack (80-bit floats)
    mmx87_regs_t mmx[8];       // MMX registers

    uint16_t    segs[6];        // CS, DS, ES, FS, GS, SS segments
    uintptr_t   segs_offs[6];   // Segment base offsets

    box64context_t *context;    // Link to global context
    int         quit;           // Exit flag
    // ... more fields
} x64emu_t;
```

**`SetupInitialStack()`** prepares the stack exactly as Linux would for a real x86_64 process:
```
[top of stack]
   NULL                    ← end marker
   envp[n] ... envp[0]     ← environment variable pointers
   NULL                    ← separator
   argv[n] ... argv[0]     ← argument pointers
   argc                    ← argument count
[RSP points here]
```

**`SetupX64Emu()`** initializes registers:
- CS = 0x33 (64-bit code segment)
- MXCSR = 0x1F80 (SSE control: mask all FP exceptions)
- x87 CW = 0x037F (FPU control word)
- RSP = aligned stack pointer
- RBP = 0 (no previous frame)

---

### STEP 7: Library Loading & Wrapping (`src/core.c:1426`)

test01 is dynamically linked and needs `libc.so.6`. Box64 **does not** load the x86_64 libc. Instead, it uses **native ARM64 wrapped libraries**:

```
[BOX64] Trying to add "libc.so.6" to maplib
[BOX64] Using native(wrapped) libc.so.6
[BOX64] Using native(wrapped) ld-linux-x86-64.so.2
[BOX64] Using native(wrapped) libpthread.so.0
[BOX64] Using native(wrapped) libdl.so.2
[BOX64] Using native(wrapped) libutil.so.1
[BOX64] Using native(wrapped) libresolv.so.2
[BOX64] Using native(wrapped) librt.so.1
[BOX64] Using native(wrapped) libbsd.so.0
```

**How wrapping works:**

For each x86_64 library function (e.g., `printf`, `malloc`, `__libc_start_main`), box64 has a **wrapper** that:
1. Reads arguments from the x86_64 calling convention (RDI, RSI, RDX, RCX, R8, R9 + stack)
2. Converts any data structures if needed (different sizes/layouts between x86_64 and native)
3. Calls the **native ARM64 libc** function
4. Converts the return value back and puts it in RAX

This is done through a **bridge mechanism**:
```
[BOX64] New Bridge brick at 0x30000000 (size 0x4000)
[BOX64] New Bridge brick at 0x30010000 (size 0x4000)
```

Bridge bricks are small code stubs at fixed addresses that:
- When the x86_64 code calls a library function via the PLT/GOT
- The call lands on a bridge address
- The bridge transitions from x86_64 emulation to native ARM64 code
- The native wrapper function executes
- Control returns to x86_64 emulation

---

### STEP 8: Relocation (`src/core.c:1433`)

After loading libraries, box64 patches the ELF's GOT (Global Offset Table) and PLT (Procedure Linkage Table) to point to the bridge addresses:

```
[BOX64] Grabbing R_X86_64_COPY Relocation(s) in advance for tests/test01
[BOX64] And now export symbols / relocation for tests/test01...
[BOX64] Applying 8 Relocation(s) with Addend for tests/test01
```

For test01, the key relocation is `__libc_start_main@GLIBC_2.34`:
- The GOT entry at `0x3fc0` (relative) is patched to point to the bridge for the native `__libc_start_main` wrapper
- When `_start` calls `__libc_start_main`, it goes through this bridge

---

### STEP 9: Start Emulation (`src/core.c:1454`)

The `emulate()` function begins execution:

```c
int emulate(x64emu_t* emu, elfheader_t* elf_header) {
    my_context->ep = GetEntryPoint(my_context->maplib, elf_header);
    atexit(endBox64);
    loadProtectionFromMap();

    SetRIP(emu, my_context->ep);          // RIP = 0x100001040 (_start)
    Push64(emu, my_context->exit_bridge); // push exit bridge as return addr
    SetRDX(emu, Pop64(emu));              // RDX = exit function pointer

    DynaRun(emu);   // <-- START EXECUTING X86_64 CODE
    ...
}
```

```
[BOX64] Entry Point is 0x100001040
[BOX64] Start x64emu on Main
```

---

### STEP 10: The Execution Engine - DynaRun (`src/dynarec/dynarec.c`)

`DynaRun()` is the heart of box64. It has **two execution modes**:

#### Mode A: Dynarec (Dynamic Recompilation) - Default

```
DynaRun(emu)
  └→ EmuRun(emu, use_dynarec=1)
       └→ while (!emu->quit) {
              block = DBGetBlock(emu, RIP, 1);

              if (block exists && complete) {
                  // Execute pre-compiled ARM64 code!
                  native_prolog(emu, block->block);
              } else {
                  // Fall back to interpreter
                  Run(emu, 1);
              }
          }
```

**How dynarec works for test01:**

1. First time RIP reaches `0x100001040` (`_start`), no compiled block exists
2. Box64's dynarec **analyzes the x86_64 instruction stream** starting at that address
3. It identifies a "basic block" - a straight-line sequence ending at a branch/call
4. It **translates each x86_64 instruction to equivalent ARM64 instructions**
5. The generated ARM64 code is stored in a cache, keyed by the x86_64 address

For example, the `_start` block (`0x100001040 - 0x100001061`):

| x86_64 Instruction | ARM64 Equivalent (conceptual) |
|---------------------|-------------------------------|
| `xor %ebp,%ebp` | `mov w29, #0` |
| `mov %rdx,%r9` | `mov x9, x2` (register mapping) |
| `pop %rsi` | `ldr x6, [sp], #8` |
| `mov %rsp,%rdx` | `mov x2, sp` (mapped) |
| `and $0xfffffffffffffff0,%rsp` | `and sp, sp, #~0xf` |
| `push %rax` | `str x0, [sp, #-8]!` |
| `call *0x2f5f(%rip)` | Bridge call to __libc_start_main |

```
[BOX64] BOX64 Dynarec: higher max_db=33
[BOX64] BOX64 Dynarec: higher max_db=57
[BOX64] BOX64 Dynarec: higher max_db=90
```

These log lines show the dynarec creating increasingly larger blocks (33, 57, 90 instructions).

#### Mode B: Interpreter Fallback (`src/emu/x64run.c`)

When dynarec can't handle an instruction or during tracing, the interpreter runs:

```c
// Simplified interpreter loop
while (1) {
    opcode = Fetch8(emu);           // Read byte at RIP
    switch (opcode) {
        case 0x55:                  // push %rbp
            Push64(emu, R_RBP);
            break;
        case 0x48:                  // REX.W prefix
            rex = fetch_rex();
            opcode2 = Fetch8(emu);
            // Handle 64-bit operation...
            break;
        case 0x0F:
            opcode2 = Fetch8(emu);
            if (opcode2 == 0x05) {  // syscall
                x64Syscall(emu);    // Handle system call!
                break;
            }
            // ... other 0F-prefixed opcodes
    }
}
```

---

### STEP 11: Executing `_start` → `__libc_start_main` Bridge

The x86_64 `_start` function at `0x100001040` does:

```asm
_start:
    xor    %ebp,%ebp             ; Clear frame pointer
    mov    %rdx,%r9              ; Save rtld_fini
    pop    %rsi                  ; argc from stack
    mov    %rsp,%rdx             ; argv from stack
    and    $-16,%rsp             ; Align stack to 16 bytes
    push   %rax                  ; Push garbage for alignment
    push   %rsp                  ; Push aligned stack
    xor    %r8d,%r8d             ; fini = NULL
    xor    %ecx,%ecx             ; init = NULL
    lea    main(%rip),%rdi       ; RDI = &main (0x100001129)
    call   *__libc_start_main    ; Call through GOT
```

When the `call` hits the GOT entry for `__libc_start_main`, it jumps to a **bridge**:

```
13075|0x100001061: Calling __libc_start_main(/lib64/libc.so.6)
    (0x100001129, 0x1, 0xFFFF6499E6C8, ...) =>
```

The bridge:
1. Stops x86_64 emulation
2. Extracts arguments from emulated registers (RDI=main, RSI=argc, RDX=argv...)
3. Calls the **native ARM64 `__libc_start_main`** from the host libc
4. The native libc performs C runtime initialization
5. Then calls back into the emulated `main()` at `0x100001129`

---

### STEP 12: Running main() and the init/fini Sequence

Before calling `main()`, the C runtime calls initialization functions:

```
[BOX64] Calling init from main elf
[BOX64] Calling Init for tests/test01 @0x100001000     ← _init function
[BOX64] Done Init for tests/test01
[BOX64] Calling Init[0] for tests/test01 @0x100001120  ← .init_array[0]
[BOX64] All Init Done for tests/test01
[BOX64] Transfert to main(1, 0xffff6499e6c8, 0xffff6499e6d8) => 0x100001129
```

Now control reaches `main()` at `0x100001129`.

---

### STEP 13: Executing main() - The String Setup

The first part of `main()` builds the message string on the stack:

```asm
1129: push   %rbp                              ; Save old frame pointer
112a: mov    %rsp,%rbp                          ; Set up new frame
112d: mov    %edi,-0x24(%rbp)                   ; Store argc
1130: mov    %rsi,-0x30(%rbp)                   ; Store argv
1134: movabs $0x3878206f6c6c6548,%rax           ; RAX = "Hello x8"
113e: mov    %rax,-0x20(%rbp)                   ; Store on stack
1142: movabs $0x726f572034365f36,%rax           ; RAX = "6_64 Wor"
114c: mov    %rax,-0x18(%rbp)                   ; Store on stack
1150: movabs $0xa21646c726f57,%rax              ; RAX = "World!\n\0"
115a: mov    %rax,-0x13(%rbp)                   ; Store on stack
```

The dynarec translates each of these to ARM64 equivalents. The `movabs` (move absolute 64-bit immediate to register) becomes a series of ARM64 `mov`/`movk` instructions since ARM64 can only load 16 bits at a time into a register.

---

### STEP 14: The Syscall (`0x10000117a`)

This is where the real magic happens. The x86_64 code sets up a Linux `write()` syscall:

```asm
115e: lea    -0x20(%rbp),%rcx     ; RCX = pointer to "Hello x86_64 World!\n"
1162: mov    $0x1,%rax             ; RAX = 1 (sys_write)
1169: mov    $0x1,%rdi             ; RDI = 1 (stdout)
1170: mov    %rcx,%rsi             ; RSI = buffer pointer
1173: mov    $0x14,%rdx            ; RDX = 20 (byte count)
117a: syscall                      ; INVOKE SYSTEM CALL
```

When the dynarec or interpreter encounters the `syscall` instruction (opcode `0x 0F 05`):

```
[BOX64] 13075|0x10000117c: Calling syscall 0x01 (1)
    0x1 0xffff6499e670 0x14 (nil) (nil) 0x30000080
```

Box64's `x64Syscall()` handler (`src/emu/x64syscall.c`) kicks in:

1. **Read syscall number from RAX**: `1` = `sys_write`
2. **Read arguments** from x86_64 registers:
   - RDI = 1 (file descriptor = stdout)
   - RSI = 0xffff6499e670 (pointer to the string in emulated stack memory)
   - RDX = 0x14 = 20 (byte count)
3. **Translate to native syscall**: The `write` syscall is straightforward - same semantics on ARM64
4. **Execute native syscall**: `write(1, buf, 20)` using the host kernel
5. **Return result in RAX**: 0x14 (20 bytes written)

```
Hello x86_64 World!
=> 0x14
```

The actual output appears on your terminal.

---

### STEP 15: Return from main()

After the syscall, execution continues:

```asm
117c: mov    $0x0,%eax    ; return value = 0
1181: pop    %rbp         ; restore frame pointer
1182: ret                 ; return to __libc_start_main
```

```
 return 0x0
```

The `ret` instruction pops the return address from the stack and jumps back to the native `__libc_start_main`, which then calls `exit(0)`.

---

### STEP 16: Cleanup & Exit

```
[BOX64] Emulation finished, EAX=0
[BOX64] Calling atexit registered functions (exiting box64)
[BOX64] Calling atexit registered functions
[BOX64] Calling fini for all loaded elfs and unload native libs
```

The exit sequence (`endBox64()` in `src/core.c:642`):

1. **Call atexit handlers**: `CallAllCleanup(emu)` - runs any registered cleanup functions
2. **Run ELF fini**: `RunElfFini()` calls the `.fini` and `.fini_array` functions:
   ```
   [BOX64] Calling Fini[0] for tests/test01 @0x1000010e0   ← __do_global_dtors_aux
   [BOX64] Calling Fini for tests/test01 @0x100001184       ← _fini
   ```
3. **Decrement library refcounts**: Unload wrapped libraries
4. **Free dynarec blocks**: Release all compiled ARM64 code:
   ```
   [BOX64] Free DynaBlocks 0x100000000-0x100004000 for tests/test01
   ```
5. **Unmap ELF memory**: Release the memory where test01 was loaded:
   ```
   [BOX64] Unmap elf memory 0x100000000-0x100008018 for tests/test01
   ```
6. **Free bridges**: Release bridge code stubs:
   ```
   [BOX64] FreeBridge brick at 0x30000000 (size 0x4000)
   ```
7. **Free emulator**: Destroy the x64emu_t structure:
   ```
   [BOX64] Free a X86_64 Emu (0x49652d20)
   ```

---

## Key Architectural Concepts

### 1. Dynarec (Dynamic Recompilation)

The dynarec is box64's performance secret. Instead of interpreting one x86_64 instruction at a time, it:

1. **Scans** a block of x86_64 instructions until it hits a branch/call/ret
2. **Translates** the entire block to ARM64 machine code in one pass
3. **Caches** the ARM64 code block, indexed by the x86_64 start address
4. **Links** blocks together - if block A jumps to block B and both are compiled, the ARM64 code jumps directly without going through the emulator loop

This is fundamentally similar to how JIT compilers work in Java or JavaScript engines.

### 2. Library Wrapping vs. Emulation

Box64 does NOT emulate x86_64 libc/libpthread/etc. Instead:

- **Wrapped (native)**: The host ARM64 libc is used directly. A thin wrapper translates the x86_64 ABI (calling convention, struct layouts) to the native ABI. This is much faster.
- **Emulated**: For libraries without wrappers, box64 loads the actual x86_64 `.so` file and emulates it instruction by instruction.

Common wrapped libraries: libc, libpthread, libdl, libm, libGL, libSDL2, libX11, etc.

### 3. Bridge Mechanism

Bridges solve the transition between emulated x86_64 code and native ARM64 code:

- **x86→native bridge**: When emulated code calls a wrapped library function, the bridge extracts arguments from emulated x86_64 registers, calls the native function, and puts the result back.
- **native→x86 bridge**: When a native callback (like a signal handler or qsort comparator) needs to call back into x86_64 code, the bridge sets up the emulated registers and re-enters the emulator.

### 4. Memory Model

Box64 runs in a single process. The emulated x86_64 code and box64's own ARM64 code share the same address space:

```
0x00000000-0x30000000  : Available for ELF loading
0x30000000-0x30090000  : Bridge bricks (library function trampolines)
0x100000000            : test01 loaded here (PIE base)
0xFFFF...              : Stack, box64 internal allocations
```

### 5. Syscall Interception

x86_64 Linux syscalls use the `syscall` instruction with:
- RAX = syscall number
- RDI, RSI, RDX, R10, R8, R9 = arguments

Box64 intercepts every `syscall` instruction. Most simple syscalls (read, write, close) are passed through directly to the kernel. Complex ones (mmap, sigaction, ioctl) need translation because:
- Struct layouts may differ between x86_64 and ARM64 conventions
- Pointer sizes and alignments may need adjustment
- Signal handling requires maintaining emulated signal contexts
- Memory mappings need to be tracked for dynarec (to know which pages contain code)

---

## Summary: Complete Lifecycle for test01

```
1. box64 starts (ARM64 binary)
2. Parse "tests/test01" command line argument
3. Open tests/test01, read ELF header → x86_64 PIE binary
4. Parse 13 program headers, 31 section headers
5. Allocate memory at 0x100000000, load 4 LOAD segments
6. Load native wrapped libc.so.6 (+ 7 more libraries)
7. Relocate: patch 8 GOT entries to point to native bridges
8. Allocate 8MB stack at 0xffff032c0000
9. Create x64emu_t with 16 GP regs + SSE + FPU state
10. Push argc/argv/envp onto emulated stack
11. Set RIP = 0x100001040 (_start)
12. DynaRun: translate _start block → ARM64, execute natively
13. _start calls __libc_start_main → bridge → native libc
14. Native libc calls _init, .init_array, then main() via bridge
15. Dynarec translates main() → ARM64 code
16. String "Hello x86_64 World!\n" assembled on stack
17. syscall instruction intercepted → x64Syscall()
18. write(1, buf, 20) executed natively → output appears
19. main returns 0 → __libc_start_main → exit(0)
20. endBox64(): run fini, free dynarec blocks, unmap memory, done
```

**Result**: `Hello x86_64 World!` printed successfully on an ARM64 machine.

---
---

# Dynarec Deep Dive: How x86_64 Code Becomes ARM64

This section explains in full detail how the dynarec (dynamic recompiler) translates x86_64 machine code into native ARM64 machine code, using real output from running test01.

## 1. Register Mapping

The dynarec permanently maps x86_64 registers to ARM64 registers. This is defined in `src/dynarec/arm64/arm64_mapping.h`:

```
x86_64 Register  →  ARM64 Register   ARM64 Alias
─────────────────────────────────────────────────
RAX              →  x10              xRAX
RCX              →  x11              xRCX
RDX              →  x12              xRDX
RBX              →  x13              xRBX
RSP              →  x14              xRSP
RBP              →  x15              xRBP
RSI              →  x16              xRSI
RDI              →  x17              xRDI
R8               →  x18              xR8
R9               →  x19              xR9
R10              →  x20              xR10
R11              →  x21              xR11
R12              →  x22              xR12
R13              →  x23              xR13
R14              →  x24              xR14
R15              →  x25              xR15
EFLAGS           →  x26              xFlags
RIP              →  x27              xRIP
(saved SP)       →  x28              xSavedSP

x64emu_t*        →  x0               xEmu
(scratch regs)   →  x1-x7            x1-x6, x87pc
(link register)  →  x30              xLR
```

**Key insight**: The 16 x86_64 general-purpose registers live permanently in ARM64 registers x10-x25 during dynarec execution. This means most register-to-register operations translate to a single ARM64 instruction with zero memory access.

ARM64 registers x1-x7 are used as scratch registers for temporary computations. x0 always holds the pointer to the `x64emu_t` structure (the emulated CPU state in memory).

---

## 2. The Prolog and Epilog

Before any dynarec block can execute, the CPU state must be loaded from the `x64emu_t` structure into ARM64 registers. After execution, it must be stored back.

### Prolog (`src/dynarec/arm64/arm64_prolog.S`)

Called via `native_prolog(emu, block)` where x0=emu pointer, x1=block address:

```asm
arm64_prolog:
    ; Save ARM64 callee-saved registers onto the native stack
    stp     x30, x29, [sp, -16]!      ; Save link register + frame pointer
    sub     sp,  sp, (8 * 18)
    stp     x19, x20, [sp, (8 * 0)]   ; Save x19-x28 (callee-saved)
    stp     x21, x22, [sp, (8 * 2)]
    ...
    stp     d14, d15, [sp, (8 *16)]   ; Save NEON d8-d15 (callee-saved)

    ; Load x86_64 registers from x64emu_t* (x0) into ARM64 registers
    ldp     x10, x11, [x0, (8 *  0)]  ; RAX, RCX
    ldp     x12, x13, [x0, (8 *  2)]  ; RDX, RBX
    ldp     x14, x15, [x0, (8 *  4)]  ; RSP, RBP
    ldp     x16, x17, [x0, (8 *  6)]  ; RSI, RDI
    ldp     x18, x19, [x0, (8 *  8)]  ; R8, R9
    ldp     x20, x21, [x0, (8 * 10)]  ; R10, R11
    ldp     x22, x23, [x0, (8 * 12)]  ; R12, R13
    ldp     x24, x25, [x0, (8 * 14)]  ; R14, R15
    ldp     x26, x27, [x0, (8 * 16)]  ; EFLAGS, RIP

    ; Save native stack pointer for later restoration
    add     x28, sp, 16               ; xSavedSP

    ; Jump to the compiled block!
    br      x1
```

### Epilog (`src/dynarec/arm64/arm64_epilog.S`)

When a block finishes and returns to the emulator loop:

```asm
arm64_epilog:
    ; Store x86_64 registers back into x64emu_t*
    stp     x10, x11, [x0, (8 *  0)]  ; RAX, RCX → emu->regs[0..1]
    stp     x12, x13, [x0, (8 *  2)]  ; RDX, RBX → emu->regs[2..3]
    ...
    stp     x26, x27, [x0, (8 * 16)]  ; EFLAGS, RIP → emu->eflags, emu->ip

    ; Restore ARM64 callee-saved registers
    add     sp, x28, 0                ; Restore native stack pointer
    ldp     x19, x20, [sp, (8 * 0)]
    ...
    ldp     x30, x29, [sp], 16        ; Restore LR

    ret                                ; Return to EmuRun() loop
```

**The flow**: `EmuRun()` → prolog (load regs) → dynarec block (ARM64 code) → epilog (store regs) → back to `EmuRun()`.

---

## 3. The 4-Pass Compilation Pipeline

Block compilation happens in `FillBlock64()` (`src/dynarec/dynarec_native.c:411`). It runs **4 passes** over the same x86_64 instruction stream, each using the same opcode decoder but different macros:

### Pass 0: Discovery (`dynarec_arm64_pass0.h`)

- **Purpose**: Decode all x86_64 instructions, find block boundaries, identify jump targets
- **`EMIT(A)`** macro: Just counts `dyn->native_size += 4` (each ARM64 instruction is 4 bytes)
- **Outputs**: Block size, jump targets list, instruction addresses, flag usage analysis

```c
uintptr_t end = native_pass0(&helper, addr, alternate, is32bits, inst_max);
```

After pass 0, box64 computes:
- **Barriers**: Where the FPU/SSE cache must be flushed (at jumps leaving the block)
- **Predecessors**: Which instructions can jump to which (for dataflow analysis)
- **Flag liveness**: Which x86_64 flags are actually needed at each instruction (critical optimization)

### Pass 1: Optimization (`dynarec_arm64_pass1.h`)

- **Purpose**: FPU/SSE register allocation, native flag optimization
- **Key optimization**: Determines which x86_64 flag computations can use ARM64's native NZCV flags directly instead of computing them in software
- **Also**: x87 FPU stack tracking, SSE register caching in NEON registers

### Pass 2: Sizing (`dynarec_arm64_pass2.h`)

- **Purpose**: Calculate exact sizes of all emitted ARM64 instructions
- **`EMIT(A)`**: Counts sizes and records per-instruction offsets
- After pass 2, box64 knows exactly how much memory to allocate for the compiled block
- If the block is too large (>MAXBLOCK_SIZE), it gets split and recompiled

```c
native_pass2(&helper, addr, alternate, is32bits, inst_max);
size_t native_size = (helper.native_size+7)&~7;  // round up
```

### Pass 3: Code Emission (`dynarec_arm64_pass3.h`)

- **Purpose**: Actually write ARM64 machine code into executable memory
- **`EMIT(A)`** macro: `*(uint32_t*)(dyn->block) = (uint32_t)(A); dyn->block += 4;`
- This is where the actual ARM64 binary code gets written

```c
// Allocate executable memory
void* actual_p = AllocDynarecMap(addr, sz, is_new);
// ... setup pointers ...
native_pass3(&helper, addr, alternate, is32bits, inst_max);
// Flush instruction cache
ClearCache(actual_p+sizeof(void*), native_size);
```

### Why 4 passes?

The same C source file (e.g., `dynarec_arm64_00.c`) is compiled 4 times with different `STEP` values. The `#include "dynarec_arm64_helper.h"` at the top selects which pass header to use:

```c
// dynarec_arm64_helper.h
#if STEP == 0
#include "dynarec_arm64_pass0.h"
#elif STEP == 1
#include "dynarec_arm64_pass1.h"
#elif STEP == 2
#include "dynarec_arm64_pass2.h"
#elif STEP == 3
#include "dynarec_arm64_pass3.h"
#endif
```

This means the instruction decoding logic is written **once** but works differently in each pass.

---

## 4. Block Memory Layout

After compilation, a block is stored in executable memory with this layout:

```
Offset  Content
────────────────────────────────────────────
0x0000  dynablock_t*         pointer to metadata
0x0008  ARM64 instructions   the actual compiled native code
  ...   (n * 4 bytes)
        Table64              array of 64-bit constants (for LDR literal)
        JmpNext block        block linking / epilog jump stubs
        InstSize array       compressed map: x86 inst size → ARM64 inst size
        Arch-specific data   native flag info per instruction
        CallRet data         call/ret optimization data
        dynablock_t          the metadata structure itself
        Relocation data      for position-dependent blocks
```

---

## 5. Concrete Translation Examples from test01

Here is the **actual dynarec output** from running test01, showing every x86_64 instruction in `_start` and its ARM64 translation:

### Example 1: `XOR %ebp, %ebp` (clear register)

```
x86_64:  31 ED               XOR Ed, Gd        @ 0x100001040
ARM64:   4a0f01ef             EOR wEBP, wEBP, wEBP
```

The x86_64 `xor ebp, ebp` (a common idiom to zero a register) maps directly to ARM64 `EOR w15, w15, w15`. Note `wEBP` = `w15` (32-bit view of x15). Since pass 0 determined that the flags set by this XOR are never read, no flag computation code is emitted.

### Example 2: `MOV %rdx, %r9` (register-to-register move)

```
x86_64:  49 89 D1             MOV Ed, Gd        @ 0x100001042
ARM64:   aa0c03f3             MOV xR9, xRDX
```

This is a 64-bit register move. `xRDX` = x12, `xR9` = x19. One ARM64 `MOV` instruction (which is an alias for `ORR x19, xzr, x12`).

### Example 3: `POP %rsi` (pop from stack)

```
x86_64:  5E                   POP reg           @ 0x100001045
ARM64:   f84085d0             LDUR xRSI, [xRSP], 0x8
```

Pop translates to a post-increment load: load 8 bytes from `[xRSP]` into `xRSI`, then add 8 to `xRSP`. This is ARM64's `LDR x16, [x14], #8` encoded as `LDUR` (unscaled offset with post-index).

### Example 4: `AND $0xFFFFFFFFFFFFFFF0, %rsp` (align stack)

```
x86_64:  48 83 E4 F0          AND Ed, Ib        @ 0x100001049
ARM64:   927cedce             AND xRSP, xRSP, 0xfffffffffffffff0
```

Align RSP to 16-byte boundary. ARM64 has a barrel shifter with immediate encoding that can represent this bitmask directly, so it's one instruction.

### Example 5: `PUSH %rax` (push to stack)

```
x86_64:  50                   PUSH reg          @ 0x10000104d
ARM64:   f81f8dca             STR xRAX, [xRSP, -0x8]!
```

Push translates to a pre-decrement store: subtract 8 from `xRSP`, then store `xRAX` at the new `xRSP`. This is ARM64's pre-index addressing mode.

### Example 6: `PUSH %rsp` (push rsp itself - special case)

```
x86_64:  54                   PUSH reg          @ 0x10000104e
ARM64:   aa0e03e1             MOV x1, xRSP          ; copy RSP to scratch
         f81f8dc1             STR x1, [xRSP, -0x8]! ; push the copy
```

When pushing RSP, the value of RSP *before* the push must be stored. So the dynarec emits 2 instructions: first copy RSP to a scratch register, then push the scratch.

### Example 7: `XOR %ecx, %ecx` (zero register - with flags needed)

```
x86_64:  31 C9                XOR Ed, Gd        @ 0x100001052
ARM64:   528006c4             MOVZ w4, 0x36          ; deferred flags type = d_xor32
         b9044c04             STR w4, [xEmu, 0x44c]  ; store to emu->df
         4a0b016b             EOR wECX, wECX, wECX   ; actual XOR
         b904600b             STR wECX, [xEmu, 0x460] ; store result for deferred flag computation
```

This XOR is different from the first one. Here, the flag analysis (pass 0/1) determined that a later instruction **needs the flags from this XOR**. So the dynarec emits the **deferred flags** mechanism: store the operation type and result into the `x64emu_t` structure, so flags can be computed lazily if actually needed.

### Example 8: `LEA main(%rip), %rdi` (RIP-relative address)

```
x86_64:  48 8D 3D CE 00 00 00   LEA Gd, Ed     @ 0x100001054
ARM64:   91006f7b             ADD xRIP, xRIP, 0x1b   ; advance RIP
         91033b71             ADD xRDI, xRIP, 0xce    ; RDI = RIP + 0xce
```

LEA computes `RIP + 0xce` (the address of `main`). The dynarec first updates the tracked RIP value, then adds the displacement.

### Example 9: `CALL *__libc_start_main` (indirect call through GOT)

```
x86_64:  FF 15 5F 2F 00 00   CALL Ed           @ 0x10000105b
ARM64:   d287f802             MOVZ x2, 0x3fc0          ; GOT offset
         f2c00022             MOVK x2, 0x1 LSL 32      ; full address = 0x100003fc0
         f9400041             LDR x1, [x2]              ; load function pointer from GOT
         91001b7b             ADD xRIP, xRIP, 0x6       ; advance RIP past this instruction
         f81f8ddb             STR xRIP, [xRSP, -0x8]!   ; push return address
         ; --- now jump to target ---
         aa0103fb             MOV xRIP, x1              ; update RIP to target
         ; --- block linkage lookup (3-level page table) ---
         aa5bc3e2             MOV x2, xRIP, LSR 48      ; check if address is too high
         35000182             CBNZ w2, #+12i             ; if so, go to epilog
         5293be03             MOVZ w3, 0x9df0            ; page table base
         72a6bf03             MOVK w3, 0x35f8 LSL 16
         d360bf62             UBFX x2, xRIP, 32, 16     ; extract bits [47:32]
         f8627863             LDR x3, [x3, x2, LSL 3]   ; level 1 lookup
         d34e7f62             UBFX x2, xRIP, 14, 18     ; extract bits [31:14]
         f8627863             LDR x3, [x3, x2, LSL 3]   ; level 2 lookup
         d3403762             UBFX x2, xRIP, 0, 14      ; extract bits [13:0]
         f8627862             LDR x2, [x3, x2, LSL 3]   ; level 3 lookup → native address
         d61f0040             BR x2                      ; jump to compiled block!
```

This is the most complex translation. The CALL:
1. Loads the target address from the GOT (2 instructions to build the 64-bit address)
2. Pushes the return address onto the emulated stack
3. Performs **block linkage**: a 3-level page table lookup that maps the x86_64 target address directly to the ARM64 compiled block address, without going back through the emulator loop

---

## 6. Flag Handling: The Deferred Flags Optimization

x86_64 sets EFLAGS (CF, ZF, SF, OF, PF, AF) on almost every arithmetic/logic instruction. ARM64 also has flags (NZCV), but they don't map 1:1 to x86_64 flags. Computing all 6 x86_64 flags after every instruction would be extremely expensive.

Box64's solution: **deferred flags** + **native flag mapping**.

### Deferred Flags

Instead of computing flags immediately, box64 stores the operation type and operands. Flags are only computed when actually needed:

```c
// In emit_add32():
IFX(X_PEND) {
    STRxw_U12(s1, xEmu, offsetof(x64emu_t, op1));   // save operand 1
    STRxw_U12(s2, xEmu, offsetof(x64emu_t, op2));   // save operand 2
    SET_DF(s3, rex.w?d_add64:d_add32b);              // save operation type
}
// ... perform the ADD ...
IFX(X_PEND) {
    STRxw_U12(s1, xEmu, offsetof(x64emu_t, res));   // save result
}
```

When flags are eventually needed, a single call to `CHECK_FLAGS()` computes them from the stored operands.

### Native Flag Optimization

Pass 0/1 performs **liveness analysis** on x86_64 flags. When the dynarec determines that:
- An x86_64 instruction sets ZF, and
- The next consumer of ZF is a conditional jump, and
- No instruction in between clobbers the ARM64 Z flag

...then the dynarec uses ARM64's native `ADDS`/`SUBS` instruction (which sets NZCV) and maps the conditional jump directly to ARM64's `B.EQ`/`B.NE`.

This is tracked via the `NF_EQ`, `NF_SF`, `NF_VF`, `NF_CF` bitmasks:

```c
// In emit_add32():
IFX(X_ZF) {
    IFNATIVE(NF_EQ) {} else {      // Can we use ARM64's native Z flag?
        CSETw(s4, cEQ);             // No: compute ZF manually
        BFIw(xFlags, s4, F_ZF, 1); // Store into xFlags register
    }
}
IFX(X_CF) {
    IFNATIVE(NF_CF) {} else {      // Can we use ARM64's native C flag?
        CSETw(s4, cCS);             // No: compute CF manually
        BFIw(xFlags, s4, F_CF, 1);
    }
}
```

When native flags **can** be used, the `IFNATIVE` block emits nothing, and the subsequent conditional branch uses the ARM64 condition code directly. This eliminates the entire flag computation for the common case.

---

## 7. The `IFX` Macro: Dead Flag Elimination

The `IFX(flags)` macro is one of the most important optimizations. It checks whether specific flags **need** to be computed at all:

```c
#define IFX(A) if((dyn->insts[ninst].x64.set_flags&(A)))
```

During pass 0, `updateNeed()` propagates flag requirements backwards through the instruction stream. If an instruction sets CF but no later instruction reads CF before it's overwritten, then `set_flags` won't include `X_CF`, and all CF computation code is skipped.

For example, in the `_start` block of test01, most XOR and AND instructions have their flags completely eliminated because nothing reads them.

---

## 8. Block Linking: The 3-Level Page Table

When a dynarec block jumps to another address, box64 needs to find the compiled ARM64 code for that address. Going back to the `EmuRun()` loop would be slow.

Instead, box64 maintains a **3-level page table** that maps x86_64 addresses → ARM64 block addresses:

```
Level 1: bits [47:32] → 64K entries
Level 2: bits [31:14] → 256K entries per L1 slot
Level 3: bits [13:0]  → 16K entries per L2 slot → ARM64 block address
```

The lookup sequence (from the CALL example above):

```asm
UBFX x2, xRIP, 32, 16      ; extract bits [47:32] of target
LDR  x3, [x3, x2, LSL 3]   ; level 1 lookup
UBFX x2, xRIP, 14, 18      ; extract bits [31:14]
LDR  x3, [x3, x2, LSL 3]   ; level 2 lookup
UBFX x2, xRIP, 0, 14       ; extract bits [13:0]
LDR  x2, [x3, x2, LSL 3]   ; level 3 → native code address
BR   x2                     ; jump directly to compiled code!
```

If the target block doesn't exist yet, the lookup returns the `native_epilog` address, which saves registers and returns to `EmuRun()` where `DBGetBlock()` will compile the new block.

The `LinkNext()` function (`src/dynarec/dynarec.c:32`) is the slow path: when a block is first reached, it compiles and links it into the page table for future fast-path lookups.

---

## 9. Self-Modifying Code Detection

If program code is modified at runtime (common in JIT-compiled languages, game anti-cheat, etc.), the cached dynarec blocks become invalid.

Box64 uses **memory protection** to detect this:

1. When a dynarec block is compiled for address range `[A, B]`, those pages are marked with `protectDB(addr, size)`
2. If anything writes to those pages, a **SIGSEGV** is generated
3. Box64's signal handler catches it, marks the affected pages as "hot" (`HotPage`)
4. The compiled blocks for those addresses are invalidated
5. The write is allowed to proceed (protection is temporarily removed)
6. Future block compilations for hot pages set `always_test = 1`, meaning the block's hash is checked before every execution

```c
// In FillBlock64():
if(is_inhotpage)
    block->always_test = 2;  // always verify block hash before running
```

The hash check (`X31_hash_code`) compares the current x86_64 bytes against what was compiled. If they differ, the block is recompiled.

---

## 10. Native Function Call Bridge (from the dynarec dump)

When a dynarec block calls a wrapped native function (like `__libc_start_main`), the generated code:

```asm
; 1. Store ALL x86_64 registers back to x64emu_t
STP  xRAX, xRCX, [xEmu]           ; regs[0..1]
STP  xRDX, xRBX, [xEmu, 0x10]    ; regs[2..3]
STP  xRSP, xRBP, [xEmu, 0x20]    ; regs[4..5]
STP  xRSI, xRDI, [xEmu, 0x30]    ; regs[6..7]
STP  xR8,  xR9,  [xEmu, 0x40]    ; regs[8..9]
STP  xR10, xR11, [xEmu, 0x50]    ; regs[10..11]
STP  xR12, xR13, [xEmu, 0x60]    ; regs[12..13]
STP  xR14, xR15, [xEmu, 0x70]    ; regs[14..15]
STR  xFlags, [xEmu, 0x80]        ; eflags

; 2. Load the wrapper function address from Table64
LDR  x7, [PC, #+offset]          ; x7 = wrapper address

; 3. Call the native wrapper (x0 = xEmu is already set)
BLR  x7                          ; call wrapper(emu)

; 4. Reload ALL x86_64 registers from x64emu_t
LDP  xRAX, xRCX, [xEmu]
LDP  xRDX, xRBX, [xEmu, 0x10]
...
LDP  xFlags, xRIP, [xEmu, 0x80]

; 5. Check if RIP changed (wrapper might have modified it)
CMP  xRIP, x3                    ; x3 = expected return RIP
B.NE epilog                      ; if different, return to emulator
```

This store-all/call/load-all sequence is the cost of crossing the x86↔native boundary. The wrapper function reads arguments from `emu->regs[]` (following x86_64 calling convention: RDI, RSI, RDX...), calls the native function with ARM64 calling convention, and stores the result back in `emu->regs[_AX]`.

---

## 11. File Organization of the ARM64 Dynarec

Each file handles a group of x86_64 opcodes:

```
dynarec_arm64_00.c      Primary opcodes 0x00-0xFF (ADD, OR, AND, SUB, XOR,
                        CMP, PUSH, POP, MOV, LEA, CALL, RET, JMP, etc.)
dynarec_arm64_0f.c      Two-byte opcodes 0x0F xx (SYSCALL, CMOVcc, SETcc,
                        MOVZX, MOVSX, BSF, BSR, BSWAP, etc.)
dynarec_arm64_66.c      Operand-size prefix (16-bit operations)
dynarec_arm64_660f.c    0x66 + 0x0F (SSE2 integer ops)
dynarec_arm64_f20f.c    0xF2 + 0x0F (SSE2 scalar double)
dynarec_arm64_f30f.c    0xF3 + 0x0F (SSE scalar float)
dynarec_arm64_f0.c      LOCK prefix (atomic operations)
dynarec_arm64_avx*.c    VEX-prefix AVX instructions

dynarec_arm64_d8.c      x87 FPU opcodes 0xD8 (FADD, FMUL, FCOM, etc.)
dynarec_arm64_d9.c      x87 FPU opcodes 0xD9 (FLD, FST, FLDZ, etc.)
...
dynarec_arm64_df.c      x87 FPU opcodes 0xDF

dynarec_arm64_emit_math.c    Emit helpers: ADD, SUB, ADC, SBB with flags
dynarec_arm64_emit_logic.c   Emit helpers: AND, OR, XOR with flags
dynarec_arm64_emit_shift.c   Emit helpers: SHL, SHR, SAR, ROL, ROR
dynarec_arm64_emit_tests.c   Emit helpers: TEST, CMP

dynarec_arm64_helper.c       ModRM decoding, address calculation
dynarec_arm64_functions.c    Utility: flag management, cache, barriers
dynarec_arm64_jmpnext.c      Block linking and jump-next stubs

arm64_emitter.h              Macros to encode every ARM64 instruction
arm64_prolog.S               Entry: load x86 regs from memory
arm64_epilog.S               Exit: store x86 regs to memory
arm64_next.S                 Block-to-block jump stubs
arm64_lock.S                 Atomic operation helpers (LDXR/STXR)
arm64_immenc.c               ARM64 immediate encoding utilities

updateflags_arm64.c          Flag computation routines
```

---

## 12. The arm64_emitter.h Macros

All ARM64 instructions are emitted using macros defined in `arm64_emitter.h`. These macros encode the instruction as a 32-bit integer and call `EMIT()`:

```c
// Examples from arm64_emitter.h (simplified):
#define ADDx_REG(Rd, Rn, Rm)    EMIT(0x8B000000 | (Rm<<16) | (Rn<<5) | Rd)
#define ADDSx_REG(Rd, Rn, Rm)   EMIT(0xAB000000 | (Rm<<16) | (Rn<<5) | Rd)
#define SUBx_REG(Rd, Rn, Rm)    EMIT(0xCB000000 | (Rm<<16) | (Rn<<5) | Rd)
#define EORw_REG(Rd, Rn, Rm)    EMIT(0x4A000000 | (Rm<<16) | (Rn<<5) | Rd)
#define MOVx_REG(Rd, Rn)        EMIT(0xAA0003E0 | (Rn<<16) | Rd)
#define LDRx_U12(Rt, Rn, imm)   EMIT(0xF9400000 | ((imm/8)<<10) | (Rn<<5) | Rt)
#define STRx_pre(Rt, Rn, imm)   EMIT(0xF8000C00 | ((imm&0x1FF)<<12) | (Rn<<5) | Rt)
#define Bcond(cond, imm19)       EMIT(0x54000000 | ((imm19&0x7FFFF)<<5) | cond)
#define BR(Rn)                   EMIT(0xD61F0000 | (Rn<<5))
#define BLR(Rn)                  EMIT(0xD63F0000 | (Rn<<5))
```

---

## Summary: Dynarec Performance Pipeline

```
 x86_64 binary code
        │
        ▼
 ┌──────────────────────┐
 │  Pass 0: Discovery   │  Decode x86, find block boundaries, jumps
 │  (count ARM64 size)  │  Flag liveness analysis
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐
 │  Pass 1: Optimize    │  FPU/SSE register allocation
 │  (native flags opt)  │  Determine native flag reuse
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐
 │  Pass 2: Sizing      │  Calculate exact ARM64 code size
 │  (allocate memory)   │  Allocate executable memory block
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐
 │  Pass 3: Emit        │  Write ARM64 machine code
 │  (generate binary)   │  Fill Table64, instsize, flush icache
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐
 │  Block cached in     │  Mapped: x86 addr → ARM64 block
 │  3-level page table  │  Hash stored for invalidation check
 └──────────┬───────────┘
            ▼
     Native execution
  (ARM64 CPU runs the
   generated code directly)
```
