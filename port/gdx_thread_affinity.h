/* Thread core placement. */

#ifndef GDX_THREAD_AFFINITY_H
#define GDX_THREAD_AFFINITY_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CORE_MAIN 1
#define GDX_CORE_AUDIO 2

int gdx_thread_affinity_pin(const char* threadName, int preferredCore);

#ifdef __cplusplus
}
#endif

#endif /* GDX_THREAD_AFFINITY_H */
