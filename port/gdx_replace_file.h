#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Moves srcPath onto dstPath, replacing dstPath when it already exists. */
int gdx_replace_file(const char* srcPath, const char* dstPath);

#ifdef __cplusplus
}
#endif
