// G-Diffuser — lightweight frame-time telemetry (GDX_PERF=1).
//
// Purpose: turn "I feel slowdowns sometimes" into attributed numbers from a normal play session.
// When GDX_PERF is set (any non-"0" value), the main loop records per-phase wall-clock times each
// frame and emits through gdx_port_logf:
//   * an immediate "[GDX perf] SPIKE ..." line whenever a frame exceeds the spike threshold,
//     with the per-phase breakdown of THAT frame (catches intermittent hitches with attribution);
//   * a "[GDX perf] summary ..." line every ~10 seconds: frame-time p50/p95/p99/max, mean per-phase
//     milliseconds, and the audio thread's tick p95/max over the same window.
// When telemetry is off every entry point is a bool early-return — no measurable cost.
//
// Telemetry is a Bucket A developer gate (port/gdx_dev_gates.h): GDX_PERF still works at launch,
// and F1 > Dev Tools > Developer gates > Scheduling > "Frame-time telemetry" toggles it live. The
// flag is re-latched from the gate once per frame in PerfFrameBegin, never mid-frame, so a toggle
// can never leave a phase Begin without its End.
//
// Note: enable "Write gdiffuser-run.log" (or set GDX_LOG=1 / GDX_TRACE=1) alongside it so the file
// log sink is open and the lines land in gdiffuser-run.log.
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
    GDX_PERF_SUB_RUN,       // the whole DrawAndRunGraphicsCommands call, summed over all sub-frames
    GDX_PERF_SUB_MIRROR,    // GdxUpdateFrameMirror (persistent frame-mirror refresh)
    // Breakdown INSIDE run (Fast3dWindow::DrawAndRunGraphicsCommands). These are nested within RUN,
    // not additional to it: gui + sframe + irun + eframe ~= run. They exist because "run" alone
    // could not answer the question that actually decides frame interpolation's fate -- whether a
    // sub-frame pass is expensive because it re-renders the game, or because it rebuilds the whole
    // ImGui frame, or because it blocks on the present. Those have completely different fixes, and
    // with M passes per 60 Hz tick the wrong guess costs a rebuild each time.
    // NOTE: deliberately placed after MIRROR. The `logic` figure is derived by subtracting
    // XLATE + RUN + MIRROR by explicit index, so appending here cannot corrupt it.
    GDX_PERF_SUB_GUI,    // Gui::StartDraw + Gui::EndDraw (a complete ImGui frame, per pass)
    GDX_PERF_SUB_SFRAME, // Interpreter::StartFrame (framebuffer/aspect setup, per pass)
    GDX_PERF_SUB_IRUN,   // Interpreter::Run (the display-list re-execution itself, per pass)
    GDX_PERF_SUB_EFRAME, // Interpreter::EndFrame (SwapBuffers; includes any vsync/latency block)
    // The WHOLE of gdx_gfx_run, so "logic" stops absorbing bridge work that is not game logic.
    // logic is derived by subtraction, and only ConvertRoot was timed (as xlate) -- so the texture-
    // cache drain, wide-cache sweep, RGBA16 range clears, persistent-allocation reset and the
    // frame-mirror refresh were all being reported as if the decomp were spending that time.
    // With this seam: bridge overhead = gfxrun - xlate - run, and real decomp logic = gametick -
    // gfxrun. Nested (it contains xlate and run), so it is excluded from the logic subtraction.
    GDX_PERF_SUB_GFXRUN,
    // Halves of the bridge overhead, to locate the 6.27 ms that gfxrun exposed. setup is everything
    // before translation begins (segment binding, cache sweeps, endianness probe, texture-cache
    // drain); post is everything after the sub-frame burst (frame flags, RGBA16 range clears,
    // persistent-allocation reset, mirror, diagnostics). Both nested inside gfxrun.
    GDX_PERF_SUB_SETUP,
    GDX_PERF_SUB_POST,
    // The end-of-task framebuffer mirror loop -- distinct from GDX_PERF_SUB_MIRROR, which times
    // only GdxUpdateFrameMirror. This loop copies every N64 framebuffer that was targeted as CIMG
    // anywhere in the task, and was the last untimed block inside POST.
    GDX_PERF_SUB_FBMIRROR,
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
