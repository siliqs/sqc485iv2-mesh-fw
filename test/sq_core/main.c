/*
 * main.c — runner for the firmware_core host tests.
 *
 * Tests are registered explicitly rather than auto-discovered: the list is the
 * inventory of what this suite promises to protect, and a test that is written
 * but never wired up here is worse than no test at all.
 */
#include <stdio.h>
#include <stdlib.h>

#include "sq_test.h"

int sq_checks, sq_failed_checks, sq_tests, sq_failed_tests, sq_case_failures;

void sq_hexdump(const char *label, const uint8_t *b, size_t n)
{
    printf("           %s:", label);
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0 && i)
            printf("\n           %*s", (int)8, "");
        printf(" %02X", b[i]);
    }
    printf("\n");
}

void sq_run(const char *name, void (*fn)(void))
{
    sq_case_failures = 0;
    sq_tests++;
    fn();
    if (sq_case_failures) {
        sq_failed_tests++;
        printf("  \033[31m✗\033[0m %s\n", name);
    } else {
        printf("  \033[32m✓\033[0m %s\n", name);
    }
}

int sq_report(void)
{
    printf("\n");
    if (sq_failed_tests) {
        printf("\033[31mFAILED\033[0m  %d/%d tests, %d/%d assertions\n", sq_failed_tests, sq_tests, sq_failed_checks, sq_checks);
        return 1;
    }
    printf("\033[32mPASSED\033[0m  %d tests, %d assertions\n", sq_tests, sq_checks);
    return 0;
}

/* ── registry ─────────────────────────────────────────────────────────────── */

#define TEST(fn) extern void fn(void);
#define GROUP(name)
#include "tests.def"
#undef TEST
#undef GROUP

int main(void)
{
    printf("\nfirmware_core host tests\n");

#define GROUP(name) printf("\n%s\n", name);
#define TEST(fn) sq_run(#fn, fn);
#include "tests.def"
#undef TEST
#undef GROUP

    return sq_report();
}
