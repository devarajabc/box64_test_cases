/*
 * libhot.so - Shared library with hot computation functions.
 *
 * When loaded via dlopen and called under box64 with dynarec enabled,
 * this library's code region gets its own mmaplist_t (per-mapping).
 * On dlclose, DelMmaplist leaks the chunks array.
 */

__attribute__((visibility("default")))
int hot_compute(int n) {
    volatile int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i * i;
        sum ^= (i << 2);
        sum += (i & 0xFF) * 3;
    }
    return sum;
}

__attribute__((visibility("default")))
int hot_compute_alt(int n) {
    volatile int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i * (i + 1) / 2;
        sum ^= (i >> 1);
    }
    return sum;
}
