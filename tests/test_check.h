#ifndef T113_TEST_CHECK_H
#define T113_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* Test conditions must still be evaluated when Release defines NDEBUG. */
#define TEST_CHECK(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "TEST_CHECK failed: %s:%d: %s\\n",           \
                          __FILE__, __LINE__, #expression);                      \
            exit(EXIT_FAILURE);                                                  \
        }                                                                        \
    } while (0)

#endif
