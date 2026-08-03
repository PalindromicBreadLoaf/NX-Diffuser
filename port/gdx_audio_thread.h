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

// Guards the threadCmdBuf handoff between the game thread (producer — AudioThread_QueueCmd,
// decomp/src/audio/disk/lib/thread.c, reached from external.c's ~20 call sites) and the audio
// thread (consumer — AudioThread_CreateTask's drain loop, same file). Both sides take it:
// gdx_audio_thread.cpp holds it across its whole per-tick production call, and thread.c's
// AudioThread_QueueCmd takes it under #ifdef PORT around the ring write — that one function is
// the chokepoint every QueueCmd* variant funnels through. RECURSIVE: a command handler inside
// the mutexed drain may re-enter QueueCmd, which a plain mutex would self-deadlock on.
// Exposed with C linkage so thread.c can take/release it without pulling <mutex> or any C++
// header into that TU. See gdx_audio_thread.cpp's MUTEX BOUNDARY comment.
void gdx_audio_ctx_lock(void);
void gdx_audio_ctx_unlock(void);

// Lock-free thread-cmd ring that carries gAudioCtx.threadCmdProcQueue's (readPos<<8 | writePos)
// tokens between the game/host thread (producer — AudioThread_ScheduleProcessCmds) and this audio
// thread (consumer — AudioThread_CreateTaskImpl's drain loop), replacing that ONE cross-thread
// queue's decomp osSendMesg/osRecvMesg so the audio thread never touches libultra's run
// queue/waiter lists. See gdx_audio_thread.cpp's CmdRing comment for the full rationale (fixes the
// Release 64DD-boot SIGABRT, engram crash/release-64dd-mfs-abort). Both keep the osSendMesg/
// osRecvMesg OS_MESG_NOBLOCK return convention: 0 = success, -1 = full (push) / empty (pop).
int gdx_audio_cmdring_push(unsigned int token);
int gdx_audio_cmdring_pop(unsigned int* out);

// Replaces AudioThread_CreateTaskImpl's per-tick osSendMesg to gAudioCtx.taskStartQueue (decomp/
// src/audio/disk/lib/thread.c). That queue has NO consumer in this port (AudioThread_WaitForAudioTask
// is never called), and the send was the LAST decomp osSendMesg the dedicated audio OS thread still
// executed every tick — the residual Release 64DD-boot SIGABRT (audio-thread osSendMesg concurrent
// with the boot fiber's SLMFSLoad->LeoSpdlMotor osSendMesg -> glibc heap/list abort; engram
// crash/release-64dd-residual, #1832). Now a plain atomic counter that touches zero libultra
// scheduler state. The accessors are diagnostic only (no consumer today).
void gdx_audio_taskstart_post(unsigned int token);
unsigned int gdx_audio_taskstart_count(void);
unsigned int gdx_audio_taskstart_last_token(void);

#ifdef __cplusplus
}
#endif
