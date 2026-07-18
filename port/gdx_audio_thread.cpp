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
#include <cstddef>
#include <cstdint>
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

// ===============================================================================================
// THREAD-CMD RING (the definitive fix for the Release 64DD SIGABRT — engram crash/release-64dd-
// mfs-abort, id 1815)
// ===============================================================================================
// THE RACE THIS REPLACES. gAudioCtx.threadCmdProcQueue is the ONE message queue that genuinely
// crosses the game/host OS thread and this dedicated audio OS thread:
//   - PRODUCER (game/host thread): decomp/src/audio/disk/lib/thread.c's AudioThread_
//     ScheduleProcessCmds, called from ~20 sites in external.c every game frame, osSendMesg's a
//     packed (readPos<<8 | writePos) token telling the consumer which slice of threadCmdBuf to
//     apply.
//   - PRODUCER (this audio thread, rare): AudioThread_CreateTaskImpl (thread.c:148) re-schedules
//     via the same ScheduleProcessCmds when a STOP command left the ring "finished".
//   - CONSUMER (this audio thread): AudioThread_CreateTaskImpl (thread.c:142) osRecvMesg-drains it
//     once per production tick.
// Routing that token through the decomp libultra osSendMesg/osRecvMesg body meant BOTH OS threads
// executed the kernel message-queue path concurrently. The gdx_mq_lock guard in n64_sched.c
// serialized the queue DATA, but the host thread still mutates the run queue / waiter lists /
// fibers via __osEnqueueAndYield/__osDispatchThread OUTSIDE that lock (only the deferred-wake
// DRAIN takes it), so the audio thread running osSendMesg's body still collided with the host's
// unlocked scheduler mutations — glibc-detected heap/list corruption (SIGABRT, si_code -6) during
// SLMFSLoad 64DD boot.
//
// THE FIX. Carry that token over a dedicated lock-free ring that touches ZERO libultra scheduler
// state. The audio thread's per-tick path therefore never calls osSendMesg/osRecvMesg/dispatch for
// this queue at all — it cannot corrupt the host's run queue because it never reaches it.
//
// RESIDUAL FIX (engram crash/release-64dd-residual, #1832): the ring above only moved
// AudioThread_ScheduleProcessCmds off the kernel. AudioThread_CreateTaskImpl STILL called
// osSendMesg every tick to post gAudioCtx.totalTaskCount to taskStartQueue — the post-rebuild core
// caught the audio thread there, concurrent with the boot fiber's SLMFSLoad->LeoSpdlMotor
// osSendMesg during the 64DD disk mount -> glibc heap/list abort. That send is now routed to a
// plain atomic counter (gdx_audio_taskstart_post below), because taskStartQueue has NO consumer in
// this port (AudioThread_WaitForAudioTask, its only reader, is never called — write-only, no host
// waiter), so the message never needed the kernel at all.
//
// The remaining audio-thread-reachable os-queue calls stay on the existing path deliberately, and
// none of them enters the libultra run-queue/waiter/fiber machinery from the audio thread:
//   - curAudioFrameDmaQueue is audio-thread-local (its producer is the inline, synchronous host-DMA
//     completion handler on this same thread, so CreateTaskImpl's NOBLOCK drain always empties it
//     first and its one OS_MESG_BLOCK loop never actually blocks; even if it did, __osEnqueueAndYield
//     spin-yields from a non-host thread — n64_sched.c — a bounded stall, never a run-queue mutation).
//   - audioResetQueue is written by CreateTaskImpl (thread.c) only when gAudioCtx.resetStatus != 0
//     (an audio-heap spec reset), i.e. never during the 64DD boot window this crash lives in; its
//     osSendMesg is left on the PORT path (which, from a non-host thread, defers the wake via
//     gdx_sched_defer_wake and does NOT mutate the run queue). It cannot be ring-converted the way
//     taskStartQueue was because its consumer (AudioThread_ResetAudioHeap, thread.c) does an
//     OS_MESG_BLOCK recv that, under the legacy-fiber kill-switch path, relies on the cooperative
//     __osEnqueueAndYield fiber switch — a lock-free ring spin would deadlock that mode. Flagged as
//     the one conditional, out-of-boot-window residual for a possible follow-up.
//
// A bounded MPMC ring (Dmitry Vyukov's algorithm) rather than a strict SPSC one because the queue
// has TWO producers (game thread + the audio-thread self-reschedule above) feeding one consumer.
// Each cell's release-store / acquire-load also publishes the producer's preceding threadCmdBuf
// writes to the consumer, so the command payload is visible after a successful pop. Initialized
// once at static-construction time (before any thread starts); never reset at runtime, so a
// concurrent AudioThread_InitMesgQueues re-init (audio-heap reset) can never race the ring — stale
// tokens are harmless because ProcessCmds NOOPs each AudioCmd slot as it applies it.
class CmdRing {
public:
    CmdRing() {
        for (size_t i = 0; i < kSize; i++) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
    }

    // Returns true on success, false if the ring is full (mirrors osSendMesg OS_MESG_NOBLOCK).
    bool Enqueue(uint32_t value) {
        Cell* cell;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueuePos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = value;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Returns true and writes *out on success, false if empty (mirrors osRecvMesg OS_MESG_NOBLOCK).
    bool Dequeue(uint32_t* out) {
        Cell* cell;
        size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = dequeuePos_.load(std::memory_order_relaxed);
            }
        }
        if (out != nullptr) {
            *out = cell->data;
        }
        cell->seq.store(pos + kMask + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kSize = 256; // power of two; >> the 4-slot decomp threadCmdProcMsgBuf
    static constexpr size_t kMask = kSize - 1;
    struct Cell {
        std::atomic<size_t> seq;
        uint32_t data;
    };
    Cell cells_[kSize];
    alignas(64) std::atomic<size_t> enqueuePos_;
    alignas(64) std::atomic<size_t> dequeuePos_;
};

CmdRing sCmdRing;

// ===============================================================================================
// TASK-START POST (residual Release 64DD SIGABRT fix — engram crash/release-64dd-residual, #1832)
// ===============================================================================================
// AudioThread_CreateTaskImpl (decomp/src/audio/disk/lib/thread.c) posted gAudioCtx.totalTaskCount
// to gAudioCtx.taskStartQueue via osSendMesg on EVERY production tick. That was the one remaining
// decomp osSendMesg the dedicated audio OS thread still executed — the post-rebuild crash core
// caught it running concurrently with the boot fiber's SLMFSLoad->LeoSpdlMotor osSendMesg during
// the 64DD disk mount, corrupting the glibc heap/list (both threads inside the libultra
// message-queue kernel; the host mutates the run queue via osStartThread OUTSIDE gdx_mq_lock).
// taskStartQueue has NO consumer in this port — AudioThread_WaitForAudioTask (its only reader) is
// never called — so the send never needed to reach the kernel at all. It is now a plain atomic
// counter: the audio thread records "a task was created" host-observably without ever entering
// osSendMesg. If a future host-side consumer ever needs the count/token, read these instead of
// re-adding a cross-thread message queue.
std::atomic<uint32_t> sTaskStartCount{ 0 };
std::atomic<uint32_t> sTaskStartLastToken{ 0 };

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

    // Clean teardown on signal-driven exit. libultraship's crash handler installs a SIGTERM/SIGINT/
    // SIGQUIT ShutdownHandler that calls exit() DIRECTLY (CrashHandler.cpp) — a window close, a
    // `kill`, or a desktop-session shutdown therefore bypasses main()'s gdx_audio_thread_stop().
    // exit() then runs static destructors, reaching this file's global sAudioThread while it is
    // still joinable, and std::thread::~thread() on a joinable thread calls std::terminate() (seen
    // on Linux as "terminate called without an active exception"). Registering the stop with
    // std::atexit fixes it: [basic.start.term] guarantees that because sAudioThread's construction
    // is sequenced before this atexit registration, the registered handler runs BEFORE
    // ~sAudioThread — so the thread is always signalled and joined first. Idempotent with main()'s
    // explicit stop (the second call no-ops on the already-joined, non-joinable thread). Registered
    // once; gdx_audio_thread_start() is called a single time at boot, but guard anyway.
    static bool sAtexitRegistered = false;
    if (!sAtexitRegistered) {
        sAtexitRegistered = true;
        std::atexit([] { gdx_audio_thread_stop(); });
    }
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

// Thread-cmd ring bridge for decomp/src/audio/disk/lib/thread.c (see the CmdRing comment above).
// gdx_audio_cmdring_push replaces AudioThread_ScheduleProcessCmds' osSendMesg; gdx_audio_cmdring_pop
// replaces AudioThread_CreateTaskImpl's drain-loop osRecvMesg. Both keep the osSendMesg/osRecvMesg
// OS_MESG_NOBLOCK return convention (0 = ok, -1 = full/empty) so thread.c's existing control flow
// is untouched.
extern "C" int gdx_audio_cmdring_push(unsigned int token) {
    return sCmdRing.Enqueue(static_cast<uint32_t>(token)) ? 0 : -1;
}

extern "C" int gdx_audio_cmdring_pop(unsigned int* out) {
    uint32_t value = 0;
    if (!sCmdRing.Dequeue(&value)) {
        return -1;
    }
    if (out != nullptr) {
        *out = value;
    }
    return 0;
}

// Replaces AudioThread_CreateTaskImpl's per-tick osSendMesg to taskStartQueue (see the TASK-START
// POST comment above). Lock-free, touches ZERO libultra scheduler state — the whole point of the
// residual 64DD SIGABRT fix. Relaxed ordering is sufficient: no other data is published through
// this counter, it is diagnostic only (taskStartQueue has no consumer in this port).
extern "C" void gdx_audio_taskstart_post(unsigned int token) {
    sTaskStartLastToken.store(static_cast<uint32_t>(token), std::memory_order_relaxed);
    sTaskStartCount.fetch_add(1, std::memory_order_relaxed);
}

// Diagnostic accessors for the task-start counter above. Not currently consumed; provided so a
// future host-side reader never needs to reintroduce a cross-thread message queue.
extern "C" unsigned int gdx_audio_taskstart_count(void) {
    return sTaskStartCount.load(std::memory_order_relaxed);
}

extern "C" unsigned int gdx_audio_taskstart_last_token(void) {
    return sTaskStartLastToken.load(std::memory_order_relaxed);
}
