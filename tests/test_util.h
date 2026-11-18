// Minimal test harness for host-side unit tests.
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures;
static int t_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        t_checks++;                                                              \
        if (!(cond)) {                                                           \
            t_failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        long long _a = (long long) (a), _b = (long long) (b);                                   \
        t_checks++;                                                                             \
        if (_a != _b) {                                                                         \
            t_failures++;                                                                       \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, %s == %lld\n", __FILE__, __LINE__, \
                #a, _a, #b, _b);                                                                \
        }                                                                                       \
    } while (0)

#define RUN(fn)                    \
    do {                           \
        int _before = t_failures;  \
        fn();                      \
        printf("%s %s\n", t_failures == _before ? "ok  " : "FAIL", #fn); \
    } while (0)

#define TEST_MAIN_END()                                                      \
    do {                                                                     \
        printf("%d checks, %d failures\n", t_checks, t_failures);            \
        return t_failures ? 1 : 0;                                           \
    } while (0)

#endif
