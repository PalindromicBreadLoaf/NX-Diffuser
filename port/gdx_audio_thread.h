// port/gdx_audio_thread.h — Phase 3: SoH-style dedicated audio production thread.
//
// See gdx_audio_thread.cpp's header comment for the full cross-thread touchpoint
// enumeration, the mutex boundary rationale, and the kill-switch semantics. Summary: a real
// std::thread now owns per-tick audio synthesis (previously split across the cooperative
// "Audio" fiber + the "Sched"/"Main" fiber, both driven off the render loop's VI tick via
// port/n64_sched.c's cooperative scheduler), so a long synchronous game-thread load
// (course/segment asset loads, the measured GMI_A..GMI_B ~131ms hitch) no longer starves
// audio production for its whole duration.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Resolves the kill switch (GDX_AUDIO_THREAD env var, default ON unless "0"; --audio-thread /
// --no-audio-thread CLI args override the env var) and, if enabled, starts the dedicated audio
// thread. Call once from port/main.cpp, after ctx->InitAudio() so the AudioPlayer backend
// exists. Safe to call before bootproc()/Audio_Init() actually run — the thread's own loop
// waits for gAudioContextInitialized before producing anything.
void gdx_audio_thread_start(int argc, char** argv);

// Signals the audio thread to stop and joins it. Call once, after the frame loop exits and
// before process teardown. No-op if the thread was never started (kill switch OFF).
void gdx_audio_thread_stop(void);

// Wakes the audio thread immediately instead of waiting for its 5ms self-pump timeout. Call
// once per rendered host frame (port/main.cpp's frame loop, alongside gdx_vi_tick()). No-op if
// the dedicated thread isn't active this run.
void gdx_audio_thread_notify_frame(void);

// True if the dedicated audio thread is this run's active producer (kill switch ON).
//   - decomp/src/sys/sys_audio.c's Audio_ThreadEntry checks this to gate its own (legacy)
//     production call, so exactly one producer ever touches gAudioCtx's task-creation state.
//   - libultraship/src/libultraship/libultra/os.cpp's osAiGetLength() checks this to decide
//     whether the old under-report cushion is still needed (only for the legacy fiber path).
int gdx_audio_thread_active(void);

// Guards the thread-cmd queue handoff between the game thread (producer —
// AudioThread_QueueCmd*/AudioThread_ScheduleProcessCmds, decomp/src/audio/disk/lib/thread.c
// + external.c, ~20 call sites) and the audio thread (consumer — AudioThread_CreateTask's
// drain loop, same file). gdx_audio_thread.cpp already takes this lock around its own
// per-tick production call; see that file's header comment for why the PRODUCER side is not
// currently locked (decomp/src/audio/disk/lib/thread.c is outside this phase's file-ownership
// scope) and exactly what a follow-up patch there needs to add. Exposed with C linkage so a
// future thread.c patch can take/release it without pulling in <mutex>/C++ headers into that
// TU.
void gdx_audio_ctx_lock(void);
void gdx_audio_ctx_unlock(void);

#ifdef __cplusplus
}
#endif
