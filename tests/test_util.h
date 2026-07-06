/* Minimal test scaffolding: counting assert macros, no framework. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                  \
    } while (0)

#define CHECK_MSG(cond, ...)                                               \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* Assert `needle` appears in NUL-terminated `hay`. */
#define CHECK_CONTAINS(hay, needle)                                        \
    do {                                                                   \
        const char *h_ = (hay);                                            \
        g_checks++;                                                        \
        if (!h_ || !strstr(h_, (needle))) {                                \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: expected \"%s\" in:\n    %s\n",          \
                   __FILE__, __LINE__, (needle), h_ ? h_ : "(null)");      \
        }                                                                  \
    } while (0)

#define CHECK_NOT_CONTAINS(hay, needle)                                    \
    do {                                                                   \
        const char *h_ = (hay);                                            \
        g_checks++;                                                        \
        if (h_ && strstr(h_, (needle))) {                                  \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: did NOT expect \"%s\" in:\n    %s\n",     \
                   __FILE__, __LINE__, (needle), h_);                      \
        }                                                                  \
    } while (0)

#endif /* TEST_UTIL_H */
