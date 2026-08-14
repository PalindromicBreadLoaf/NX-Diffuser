#include "gdx_replace_file.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

#ifndef _WIN32

#include <errno.h>
#include <stdio.h>

int gdx_replace_file(const char* srcPath, const char* dstPath) {
    if (srcPath == NULL || dstPath == NULL) {
        return 0;
    }
    if (rename(srcPath, dstPath) == 0) {
        return 1;
    }
    if (errno != EEXIST) {
        return 0;
    }
    if (remove(dstPath) != 0) {
        return 0;
    }
    return rename(srcPath, dstPath) == 0;
}

#endif /* !_WIN32 */

void gdx_storage_commit(void) {
#if defined(__SWITCH__)
    fsdevCommitDevice("sdmc");
#endif
}
