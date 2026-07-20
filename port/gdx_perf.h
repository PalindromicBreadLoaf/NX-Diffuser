// G-Diffuser — lightweight frame-time telemetry (GDX_PERF=1).
//
// Purpose: turn "I feel slowdowns sometimes" into attributed numbers from a normal play session.
// When GDX_PERF is set (any non-"0" value), the main loop records per-phase wall-clock times each
// frame and emits through gdx_port_logf:
//   * an immediate "[GDX perf] SPIKE ..." line whenever a frame exceeds the spike threshold,
//     with the per-phase breakdown of THAT frame (catches intermittent hitches with attribution);
//   * a "[GDX perf] summary ..." line every ~10 seconds: frame-time p50/p95/p99/max, mean per-phase
//     milliseconds, and the audio thread's tick p95/max over the same window.
// When GDX_PERF is unset every entry point is a cached-bool early-return — no measurable cost.
//
// Note: set GDX_LOG=1 (or GDX_TRACE=1) alongside GDX_PERF so the file log sink is enabled and the
// lines land in gdiffuser-run.log.
#pragma once

// ---------------------------------------------------------------------------------------------
// Second-level (sub-phase) breakdown of the game frame — C-callable so C translation units
// (n64_vi.c / n64_sched.c) and the C++ gfx bridge can share the same seams.
//
// The game's fibers do NOT run inside gdx_dispatch(): posting the VI retrace message in
// gdx_vi_tick() (main.cpp's "gametick" phase) wakes the Main scheduler thread and the cooperative
// scheduler dispatches the game fiber RIGHT THERE (osSendMesg -> osStartThread -> __osDispatchThread
// when __osRunningThread == NULL). The whole game frame — game logic AND the synchronous gfx-task
// submission (gdx_gfx_run: DL translation, interpreter Run, frame-mirror) — therefore executes
// inside the "gametick" phase. These sub-timers attribute WHERE that time goes.
//
// "logic" is not a seam; it is derived as gametick - (xlate + run + mirror). All three seams live
// on the host/main thread (the gfx task runs inside the host-thread fiber dispatch), so the timers
// need no lock; a main-thread guard skips any stray off-thread call rather than corrupt state.
// Zero cost when GDX_PERF is unset (cached-bool early return).
#ifdef __cplusplus
extern "C" {
#endif

enum GdxPerfSub {
    GDX_PERF_SUB_XLATE = 0, // DL translation (N64DisplayListAdapter::ConvertRoot in gdx_gfx_run)
    GDX_PERF_SUB_RUN,       // Fast::Interpreter::Run (the interp->Run call)
    GDX_PERF_SUB_MIRROR,    // GdxUpdateFrameMirror (persistent frame-mirror refresh)
    GDX_PERF_SUB_COUNT
};

void gdx_perf_sub_begin(int id);
void gdx_perf_sub_end(int id);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
namespace gdx {

enum PerfPhase {
    PerfEvents = 0, // HandleEvents (SDL pump, drop events, window messages)
    PerfInput,      // controller poll + aspect tick + audio notify + mouse/nav (cheap, no game work)
    PerfGameTick,   // gdx_vi_tick: posts retrace -> runs the Main game fiber (game logic + gfx submit)
    PerfGuiStart,   // Gui::StartDraw + Window::StartFrame + deferred wake drain
    PerfDispatch,   // gdx_dispatch: residual fibers only (normally ~0 — the frame ran in gametick)
    PerfTicks,      // savestate tick + disk-save tick
    PerfPresent,    // vi present fallback + Gui::EndDraw + Window::EndFrame (includes vsync wait)
    PerfPacer,      // optional frame pacer sleep
    PerfPhaseCount
};

bool PerfEnabled();

void PerfFrameBegin();
void PerfPhaseBegin(PerfPhase p);
void PerfPhaseEnd(PerfPhase p);
void PerfFrameEnd();

// Called from the dedicated audio thread with one tick's duration. Thread-safe; no-op when disabled.
void PerfAudioTick(double ms);

} // namespace gdx
#endif // __cplusplus
