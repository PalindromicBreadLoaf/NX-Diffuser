// port/gdx_audio_thread.cpp — Phase 3: SoH-style dedicated audio production thread.
//
// THE PROBLEM (measured, see engram design/audio-pipeline-hm-ports)
// ------------------------------------------------------------------------------------------
// Before this file: audio production ran as a cooperative "Audio" fiber (decomp/src/sys/
// sys_audio.c's Audio_ThreadEntry), woken once per VI tick alongside every other decomp
// thread by port/n64_sched.c's single-OS-thread scheduler, driven from port/main.cpp's frame
// loop. Actual PCM synthesis (the software RSP interpreter, port/n64_audio_hle.c) ran on yet
// a THIRD fiber (decomp/src/sys/sys_main.c's "Sched"/"Main" thread, via its
// EVENT_MESG_AUDIO_TASK_SET handler -> Sched_SpTaskStartAudio -> osSpTaskStart ->
// port/n64_sched.c's osSpTaskStartGo). All three fibers share ONE real OS thread with no
// preemption. When the GAME thread's fiber runs a long synchronous, non-yielding stretch
// (Segment_LoadAssets during a course/menu transition — measured up to ~131ms, see
// docs/IMPLEMENTATION_PLAN.md Phase 4's GMI_A..GMI_B note), NOTHING else on that one OS
// thread can run either, including the Audio/Sched fibers — audio production stops dead for
// the whole stretch. A 2048-frame (64ms) cushion in osAiGetLength (libultraship/.../os.cpp)
// papered over ordinary host-scheduling jitter but could never survive a 131ms hitch; the
// measured result was 725 holes of ~10ms silence in a 113s capture.
//
// THE FIX: give audio a REAL, independent OS thread (this file), matching HarbourMasters/
// Shipwright's proven architecture (OTRGlobals.cpp's OTRAudio_Thread). Because it is a
// genuinely separate OS thread, the game thread being wedged inside a long synchronous load
// no longer stops it from running.
//
// ===============================================================================================
// CROSS-THREAD TOUCHPOINT ENUMERATION (design point 1 — read this before changing anything here)
// ===============================================================================================
// PRODUCERS — game-thread-side code that mutates gAudioCtx's command-queue state. Reached only
// through decomp/src/audio/disk/lib/thread.c's public API (never gAudioCtx.threadCmdBuf /
// threadCmdWritePos / threadCmdReadPos directly):
//   - AudioThread_QueueCmd / QueueCmdF32 / QueueCmdU32 / QueueCmdS8 / QueueCmdU16
//     (thread.c:367-432): write one AudioCmd into
//     gAudioCtx.threadCmdBuf[threadCmdWritePos & 0xFF], then increment threadCmdWritePos.
//   - AudioThread_ScheduleProcessCmds (thread.c:434-467): reads threadCmdReadPos/
//     threadCmdWritePos, osSendMesg's the packed (readPos<<8 | writePos) pair to
//     gAudioCtx.threadCmdProcQueue, then commits threadCmdReadPos = threadCmdWritePos. Called
//     from ~20 sites in decomp/src/audio/disk/external.c — the Audio_SeqCmd*/Audio_SetVolume/
//     Audio_Player*-style public API game logic calls every frame. THIS is the real
//     game-thread entry point; QueueCmd alone never crosses threads without it.
//   - AudioThread_ResetAudioHeap (thread.c:624-647) / AudioThread_PreNMIInternal
//     (thread.c:651-657): also touch gAudioCtx.resetStatus/specId/resetTimer, and call
//     ScheduleProcessCmds themselves.
// CONSUMER — audio-side drain, reached only via AudioThread_CreateTask -> CreateTaskImpl
// (thread.c:18-196), which this file calls once per production tick (see
// gdx_audio_produce_one_tick in port/n64_sched.c):
//   - thread.c:141-150: while (osRecvMesg(threadCmdProcQueueP, ...) != -1)
//     AudioThread_ProcessCmds(...) — drains the ring from the LAST message's snapshot readPos
//     up to its writePos, applying each AudioCmd to gAudioCtx.seqPlayers/channels
//     (thread.c:474-557).
//   - Also audio-side and self-contained (verified by reading decomp/src/audio/disk/lib/
//     load.c — design point 7's explicit ask): AudioLoad_DecreaseSampleDmaTtls,
//     AudioLoad_ProcessLoads, AudioLoad_ProcessScriptLoads. These touch ONLY gAudioCtx's own
//     heap pool (AudioHeap_*) and its own DMA-completion queues (curAudioFrameDmaQueue,
//     syncDmaQueue, externalLoadQueue — all gAudioCtx members) via sDmaHandler/sLeoHandler
//     (already wired for synchronous host-DMA completion). NONE of them touch the game's
//     segment/asset arena (Arena_Allocate, Segment_LoadAssets) — that arena is owned
//     exclusively by the GAME thread's loaders. CONFIRMED: the audio thread's own work is
//     self-contained and never depends on anything a long synchronous game-thread load would
//     be mutating, so ProcessLoads is safe to keep running on ticks driven from this thread
//     (no need to move it back to a game-thread-driven tick).
//   - osAiSetNextBuffer (thread.c:63, forwards to libultraship's SDLAudioPlayer) and
//     AudioSynth_Update (thread.c:153, decomp/src/audio/disk/lib/synthesis.c) — also touch
//     only gAudioCtx-owned buffers (aiBuffers[3], curAbiCmdBuf).
//   - gdx_audio_hle_run (port/n64_audio_hle.c) — executes the Acmd list AudioSynth_Update just
//     built. Its state (sDmem, sAdpcmBook) is a private static in that TU, touched only from
//     this call path; the game thread never touches it.
//
// A SECOND, SEPARATE HAZARD found while enumerating (a fiber-affinity crash risk, not a
// gAudioCtx data race): AudioThread_CreateTaskImpl (thread.c:82) and several
// AudioLoad_Sync* functions reachable through AudioThread_ProcessGlobalCmd
// (thread.c:207-325, e.g. AUDIOCMD_OP_GLOBAL_SYNC_LOAD_*) call
// osRecvMesg(..., OS_MESG_BLOCK) on gAudioCtx's own DMA-completion queues. On a blocking wait
// against an empty queue, decomp/src/libultra/os/recvmesg.c calls __osEnqueueAndYield ->
// __osDispatchThread -> SwitchToFiber (port/n64_sched.c) — and Win32 fibers are only valid on
// the OS thread that converted itself via ConvertThreadToFiber (the original host/main
// thread). If THIS dedicated audio thread — a real, separate std::thread — ever reached one
// of those blocking waits, calling into the fiber scheduler would corrupt or crash it. In
// this port these waits are expected to never actually block (every DMA handler this port
// wires up completes synchronously and posts its completion message inline, before
// CreateTaskImpl's preceding NOBLOCK drain even runs), but that is an assumption about
// decomp/port-glue behavior this file does not control on its own. Mitigated defensively in
// port/n64_sched.c: __osEnqueueAndYield now checks the calling OS thread against the host
// thread ID recorded by gdx_sched_init() and, if it doesn't match, logs once (rate-limited)
// and returns (spin-yield) instead of touching the fiber scheduler — turning a would-be crash
// into a bounded, diagnosable stall.
//
// MUTEX BOUNDARY (design point 6)
// ------------------------------------------------------------------------------------------
// A mutex that closes the PRODUCER side above would need lock/unlock calls inside
// AudioThread_QueueCmd*/ScheduleProcessCmds/ProcessCmds — decomp/src/audio/disk/lib/thread.c,
// which is OUTSIDE this phase's file-ownership scope (see this change's task description; the
// owned file list is deliberately narrow). What IS implemented here: sAudioCtxMutex, held for
// this file's ENTIRE per-tick production call (gdx_audio_produce_one_tick(), which itself
// calls AudioThread_CreateTask() + gdx_audio_hle_run() — see port/n64_sched.c). A future
// thread.c patch only needs to wrap its own three call sites with the SAME lock
// (gdx_audio_ctx_lock/unlock, exported with C linkage from gdx_audio_thread.h) to close the
// loop completely. Until that lands, the residual, KNOWN, UNCLOSED risk is: the game thread's
// AudioThread_QueueCmd writes (thread.c:367-378 — a handful of word stores) can race with this
// thread's drain reads with no memory-ordering guarantee between them. Bounded impact if hit
// (x86/x64's strong store ordering plus the rarity/brevity of the write window means the
// realistic failure mode is a single garbled AudioCmd — a one-tick audio glitch — not a
// crash), but it is real and currently unclosed. Flagged prominently in this phase's report
// for the orchestrator to decide: accept for this phase's A/B, or schedule the thread.c
// follow-up first.
//
// KILL SWITCH (design point 5)
// ------------------------------------------------------------------------------------------
// GDX_AUDIO_THREAD env var (default ON unless set to "0") / --audio-thread / --no-audio-thread
// CLI args (checked in that order, CLI wins — mirrors the --seed-boot-logo /
// --diag-settimg pattern already used in port/n64_gfx_bridge.cpp). OFF reverts completely to
// the legacy fiber-driven path: decomp/src/sys/sys_audio.c's Audio_ThreadEntry resumes
// producing every VI tick, and libultraship/.../os.cpp's osAiGetLength keeps its original
// 2048-frame under-report cushion — i.e. the pre-Phase-3 behavior, byte-for-byte, for a clean
// A/B comparison.
// ===============================================================================================

#include "gdx_audio_thread.h"
#include "port_log.h"
#include "libultraship/bridge/audiobridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

// Defined in port/n64_sched.c, which already includes the decomp audio headers this needs
// (AudioTask/OSTask/gAudioContextInitialized) — see that file's own comment for why this TU
// stays decomp-header-free (matches port/n64_audio_hle.c's existing precedent).
extern "C" int gdx_audio_produce_one_tick(void);

namespace {

// Guards decomp's gAudioCtx thread-cmd queue handoff — see the MUTEX BOUNDARY comment above
// for exactly what this does and does not protect yet.
// Recursive: AudioThread_QueueCmd (decomp thread.c) takes this lock at the producer
// chokepoint; if any command HANDLER inside the mutexed drain ever re-queues a command,
// a plain mutex would self-deadlock. Recursion makes that whole class impossible.
std::recursive_mutex sAudioCtxMutex;

std::mutex sWakeMutex;
std::condition_variable sWakeCv;
std::thread sAudioThread;
std::atomic<bool> sStopRequested{ false };
std::atomic<bool> sThreadActive{ false }; // resolved kill-switch value, cached for this run

// Safety cap on ticks produced per wake. AudioThread_CreateTaskImpl (thread.c) can legitimately
// return without producing a task (its own totalTaskCount % specUnk4 gating) — that is not a
// terminal condition for the catch-up loop, just "not yet", so the loop does not break early on
// it. This cap only exists to bound worst-case per-wake CPU work if something upstream keeps
// reporting Buffered() below DesiredBuffered indefinitely (e.g. during the brief window before
// the SDL device is fully warmed up).
constexpr int kMaxTicksPerWake = 64;

bool ResolveKillSwitch(int argc, char** argv) {
    bool enabled = true;
    if (const char* env = std::getenv("GDX_AUDIO_THREAD")) {
        enabled = (env[0] != '0');
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i] == nullptr) {
            continue;
        }
        if (std::strcmp(argv[i], "--no-audio-thread") == 0) {
            enabled = false;
        } else if (std::strcmp(argv[i], "--audio-thread") == 0) {
            enabled = true;
        }
    }
    return enabled;
}

void AudioThreadMain() {
    using namespace std::chrono_literals;
    gdx_port_logf("[audio-thread] dedicated audio thread started (DesiredBuffered=%d)\n",
                  AudioPlayerGetDesiredBuffered());

    while (!sStopRequested.load(std::memory_order_relaxed)) {
        {
            // 5ms self-pump timeout (design point 2): survives render stalls/loads on the main
            // thread — this thread never depends on gdx_audio_thread_notify_frame() actually
            // being called in a timely fashion, only on it eventually being called OR this
            // timeout firing, whichever comes first.
            std::unique_lock<std::mutex> wakeLock(sWakeMutex);
            sWakeCv.wait_for(wakeLock, 5ms);
        }
        if (sStopRequested.load(std::memory_order_relaxed)) {
            break;
        }

        const int32_t desired = AudioPlayerGetDesiredBuffered();
        int iterations = 0;
        while (AudioPlayerBuffered() < desired && iterations < kMaxTicksPerWake) {
            {
                std::lock_guard<std::recursive_mutex> ctxLock(sAudioCtxMutex);
                gdx_audio_produce_one_tick();
            }
            iterations++;
        }
    }

    gdx_port_logf("[audio-thread] dedicated audio thread stopping\n");
}

} // namespace

extern "C" void gdx_audio_thread_start(int argc, char** argv) {
    const bool enabled = ResolveKillSwitch(argc, argv);
    sThreadActive.store(enabled, std::memory_order_relaxed);
    gdx_port_logf(
        "[audio-thread] GDX_AUDIO_THREAD -> dedicated thread %s (legacy fiber path %s)\n",
        enabled ? "ACTIVE" : "inactive",
        enabled ? "disabled (see sys_audio.c's gdx_audio_thread_active() gate)"
                : "ACTIVE (pre-Phase-3 behavior, 2048-frame osAiGetLength cushion intact)");

    if (!enabled) {
        return; // Kill switch OFF: nothing else in this file runs this session.
    }

    sStopRequested.store(false, std::memory_order_relaxed);
    sAudioThread = std::thread(AudioThreadMain);
}

extern "C" void gdx_audio_thread_stop(void) {
    if (!sThreadActive.load(std::memory_order_relaxed)) {
        return;
    }
    sStopRequested.store(true, std::memory_order_relaxed);
    sWakeCv.notify_all();
    if (sAudioThread.joinable()) {
        sAudioThread.join();
    }
}

extern "C" void gdx_audio_thread_notify_frame(void) {
    if (!sThreadActive.load(std::memory_order_relaxed)) {
        return;
    }
    // No predicate flag: a notify lost to a race just means the next production happens on the
    // 5ms self-pump timeout instead of immediately — within the tolerance the design already
    // budgets for, not a correctness issue.
    sWakeCv.notify_one();
}

extern "C" int gdx_audio_thread_active(void) {
    return sThreadActive.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void gdx_audio_ctx_lock(void) {
    sAudioCtxMutex.lock();
}

extern "C" void gdx_audio_ctx_unlock(void) {
    sAudioCtxMutex.unlock();
}
