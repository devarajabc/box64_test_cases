/*
 * Test case for x64run0f.c opcode 0x0F 0x00 missing braces bug
 *
 * Bug: In case 0x00 (SLDT/STR/VERR/VERW), the else branch at line 89
 * is missing braces, causing the second switch to always execute.
 *
 * In 32-bit mode: Both switches execute, second switch returns 0 for STR/VERR/VERW
 * In 64-bit mode: Second switch only handles SLDT (case 0), not STR/VERR/VERW
 *
 * This test executes STR (Store Task Register) instruction which should
 * work but fails due to the bug.
 */

#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>

static sigjmp_buf jump_buffer;
static volatile int got_signal = 0;

static void signal_handler(int sig) {
    got_signal = sig;
    siglongjmp(jump_buffer, 1);
}

/*
 * STR - Store Task Register (0F 00 /1)
 * Stores the segment selector from the task register (TR) into destination.
 * This is a privileged read of system state, but reading is allowed in user mode.
 *
 * Encoding: 0F 00 /1 -> 0F 00 C8 (STR AX) or 0F 00 0D [mem] (STR [mem])
 */
static int test_str_instruction(void) {
    uint16_t tr_value = 0xFFFF;  /* Initialize to known value */

    printf("Testing STR instruction (0F 00 /1)...\n");
    printf("  Initial value: 0x%04x\n", tr_value);

    /*
     * STR stores to r/m16 (or r/m32/r/m64 with size prefix)
     * 0F 00 /1 with ModRM = 0x0D for memory operand
     *
     * Using inline asm to execute: str %ax
     * Opcode bytes: 0F 00 C8 (STR AX, where C8 = 11 001 000 = mod=3, reg=1, rm=0)
     */
    __asm__ volatile (
        "str %0"
        : "=r" (tr_value)
        :
        : "memory"
    );

    printf("  After STR: 0x%04x\n", tr_value);

    /* TR value should be different from our initial 0xFFFF (typically 0x40 or similar) */
    if (tr_value != 0xFFFF) {
        printf("  STR instruction executed successfully!\n");
        return 0;  /* Success */
    } else {
        printf("  WARNING: TR value unchanged, instruction may not have executed\n");
        return 1;  /* Possible failure */
    }
}

/*
 * SLDT - Store Local Descriptor Table Register (0F 00 /0)
 * This should work even with the bug (case 0 is handled in both switches)
 */
static int test_sldt_instruction(void) {
    uint16_t ldtr_value = 0xFFFF;

    printf("Testing SLDT instruction (0F 00 /0)...\n");
    printf("  Initial value: 0x%04x\n", ldtr_value);

    __asm__ volatile (
        "sldt %0"
        : "=r" (ldtr_value)
        :
        : "memory"
    );

    printf("  After SLDT: 0x%04x\n", ldtr_value);
    printf("  SLDT instruction executed successfully!\n");
    return 0;
}

/*
 * VERR - Verify Segment for Reading (0F 00 /4)
 * Sets ZF=1 if segment selector is valid and readable
 */
static int test_verr_instruction(void) {
    uint16_t selector;
    uint8_t zf_set = 0;

    printf("Testing VERR instruction (0F 00 /4)...\n");

    /* Get current CS selector */
    __asm__ volatile ("mov %%cs, %0" : "=r" (selector));
    printf("  Testing selector: 0x%04x (CS)\n", selector);

    /* VERR sets ZF if selector is valid and readable */
    __asm__ volatile (
        "verr %1\n\t"
        "setz %0"
        : "=r" (zf_set)
        : "r" (selector)
        : "cc"
    );

    printf("  VERR result: ZF=%d (1=readable, 0=not readable)\n", zf_set);
    printf("  VERR instruction executed successfully!\n");
    return 0;
}

/*
 * VERW - Verify Segment for Writing (0F 00 /5)
 * Sets ZF=1 if segment selector is valid and writable
 */
static int test_verw_instruction(void) {
    uint16_t selector;
    uint8_t zf_set = 0;

    printf("Testing VERW instruction (0F 00 /5)...\n");

    /* Get current DS selector */
    __asm__ volatile ("mov %%ds, %0" : "=r" (selector));
    printf("  Testing selector: 0x%04x (DS)\n", selector);

    /* VERW sets ZF if selector is valid and writable */
    __asm__ volatile (
        "verw %1\n\t"
        "setz %0"
        : "=r" (zf_set)
        : "r" (selector)
        : "cc"
    );

    printf("  VERW result: ZF=%d (1=writable, 0=not writable)\n", zf_set);
    printf("  VERW instruction executed successfully!\n");
    return 0;
}

typedef int (*test_func_t)(void);

struct test_case {
    const char *name;
    test_func_t func;
};

static struct test_case tests[] = {
    { "SLDT (0F 00 /0) - should pass even with bug", test_sldt_instruction },
    { "STR  (0F 00 /1) - fails with bug", test_str_instruction },
    { "VERR (0F 00 /4) - fails with bug", test_verr_instruction },
    { "VERW (0F 00 /5) - fails with bug", test_verw_instruction },
};

int main(void) {
    int passed = 0;
    int failed = 0;
    int total = sizeof(tests) / sizeof(tests[0]);

    printf("==============================================\n");
    printf("Test: 002_0f00_missing_braces\n");
    printf("Bug: Missing braces in x64run0f.c case 0x00\n");
    printf("==============================================\n\n");

    /* Set up signal handlers for SIGILL/SIGSEGV */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    for (int i = 0; i < total; i++) {
        printf("----------------------------------------\n");
        printf("Test %d: %s\n", i + 1, tests[i].name);
        printf("----------------------------------------\n");

        got_signal = 0;

        if (sigsetjmp(jump_buffer, 1) == 0) {
            int result = tests[i].func();
            if (result == 0) {
                printf("Result: PASSED\n\n");
                passed++;
            } else {
                printf("Result: FAILED (returned %d)\n\n", result);
                failed++;
            }
        } else {
            printf("Result: CRASHED (signal %d: %s)\n\n",
                   got_signal,
                   got_signal == SIGILL ? "SIGILL - Illegal instruction" :
                   got_signal == SIGSEGV ? "SIGSEGV - Segmentation fault" :
                   "Unknown signal");
            failed++;
        }
    }

    printf("==============================================\n");
    printf("Summary: %d/%d passed, %d failed\n", passed, total, failed);
    printf("==============================================\n");

    if (failed > 0) {
        printf("\nNOTE: Failures in STR/VERR/VERW indicate the bug is present.\n");
        printf("The bug is missing braces in src/emu/x64run0f.c lines 89-101:\n");
        printf("  } else\n");
        printf("      nextop = F8;  // <-- only this is in else\n");
        printf("      switch(...) { // <-- this always runs!\n");
        printf("\nFix: Add braces around the else block.\n");
    }

    return failed > 0 ? 1 : 0;
}
