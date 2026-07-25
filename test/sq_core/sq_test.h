/*
 * sq_test.h — minimal zero-dependency assertion framework for the firmware_core
 * host tests. Deliberately NOT Unity: these tests must build with nothing but a
 * C11 compiler so `make test` works on a bare macOS / Linux box with no
 * PlatformIO, no Docker, no portduino.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── runner state (defined in main.c) ─────────────────────────────────────── */
extern int sq_checks;        /* assertions executed          */
extern int sq_failed_checks; /* assertions that failed       */
extern int sq_tests;         /* test functions run           */
extern int sq_failed_tests;  /* test functions with a failure */
extern int sq_case_failures; /* failures inside the current test */

void sq_run(const char *name, void (*fn)(void));
int sq_report(void);

/* ── failure reporting ────────────────────────────────────────────────────── */
#define SQ__FAILED(...)                                                                                                          \
    do {                                                                                                                         \
        sq_failed_checks++;                                                                                                      \
        sq_case_failures++;                                                                                                      \
        printf("      \033[31mFAIL\033[0m %s:%d\n           ", __FILE__, __LINE__);                                              \
        printf(__VA_ARGS__);                                                                                                     \
        printf("\n");                                                                                                            \
    } while (0)

void sq_hexdump(const char *label, const uint8_t *b, size_t n);

/* ── assertions ───────────────────────────────────────────────────────────── */
#define ASSERT_TRUE(cond)                                                                                                        \
    do {                                                                                                                         \
        sq_checks++;                                                                                                             \
        if (!(cond))                                                                                                             \
            SQ__FAILED("expected true: %s", #cond);                                                                              \
    } while (0)

#define ASSERT_FALSE(cond)                                                                                                       \
    do {                                                                                                                         \
        sq_checks++;                                                                                                             \
        if ((cond))                                                                                                              \
            SQ__FAILED("expected false: %s", #cond);                                                                             \
    } while (0)

/* Signed/unsigned integer equality. Values are printed both ways so an
   unexpected 0xFF vs -1 style mismatch is obvious. */
#define ASSERT_EQ(expected, actual)                                                                                              \
    do {                                                                                                                         \
        sq_checks++;                                                                                                             \
        long long _e = (long long)(expected), _a = (long long)(actual);                                                          \
        if (_e != _a)                                                                                                            \
            SQ__FAILED("%s: expected %lld (0x%llX), got %lld (0x%llX)", #actual, _e, (unsigned long long)_e, _a,                 \
                       (unsigned long long)_a);                                                                                  \
    } while (0)

#define ASSERT_STR_EQ(expected, actual)                                                                                          \
    do {                                                                                                                         \
        sq_checks++;                                                                                                             \
        const char *_e = (expected), *_a = (actual);                                                                             \
        if (strcmp(_e, _a) != 0)                                                                                                 \
            SQ__FAILED("%s: expected \"%s\", got \"%s\"", #actual, _e, _a);                                                      \
    } while (0)

/* Byte-exact buffer comparison — dumps both sides on mismatch, which is what
   makes wire-format regressions readable. */
#define ASSERT_MEM_EQ(expected, actual, len)                                                                                     \
    do {                                                                                                                         \
        sq_checks++;                                                                                                             \
        const uint8_t *_e = (const uint8_t *)(expected), *_a = (const uint8_t *)(actual);                                        \
        size_t _n = (size_t)(len);                                                                                               \
        if (memcmp(_e, _a, _n) != 0) {                                                                                           \
            SQ__FAILED("%s: %zu bytes differ", #actual, _n);                                                                     \
            sq_hexdump("expected", _e, _n);                                                                                      \
            sq_hexdump("actual  ", _a, _n);                                                                                      \
        }                                                                                                                        \
    } while (0)

#define ASSERT_FLOAT_EQ(expected, actual)                                                                                        \
    do {                                                                                                                         \
        sq_checks++;                                                                                                             \
        double _e = (double)(expected), _a = (double)(actual);                                                                   \
        double _d = _e - _a;                                                                                                     \
        if (_d < 0)                                                                                                              \
            _d = -_d;                                                                                                            \
        if (_d > 1e-6)                                                                                                           \
            SQ__FAILED("%s: expected %f, got %f", #actual, _e, _a);                                                              \
    } while (0)
