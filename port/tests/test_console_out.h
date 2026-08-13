/*
 * Test output on a platform with no console.
 */
#ifndef GDX_TEST_CONSOLE_OUT_H
#define GDX_TEST_CONSOLE_OUT_H

#include <stdio.h>

static void gdx_test_console_out(const char* logName) {
#if defined(__SWITCH__)
    if (freopen(logName, "w", stdout) == NULL) {
        return;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
#else
    (void) logName;
#endif
}

#endif /* GDX_TEST_CONSOLE_OUT_H */
