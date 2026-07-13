// gdx_frame_pacer.c -- optional wall-clock frame pacer for the host loop.
//
// GROUND TRUTH (why this defaults OFF)
// ------------------------------------
// The host loop in main.cpp advances the simulation by exactly one VI tick per
// iteration, and the loop's cadence is set by w->EndFrame(). On this port that
// resolves to Interpreter::EndFrame() -> GfxWindowBackend*::SwapBuffersBegin(),
// and BOTH shipped backends already contain a hybrid sleep+spin software frame
// limiter that holds the present rate to mTargetFps (initialised to 60), fully
// independent of VSync:
//   - DXGI/DX11 : gfx_dxgi.cpp  SwapBuffersBegin() -- SetWaitableTimer coarse
//                 sleep to ~1.5 ms before a 1e9/60 ns deadline, then a
//                 YieldProcessor spin, then Present(vsync).
//   - SDL2/GL   : gfx_sdl2.cpp  SyncFramerateWithTime() -- same shape.
// Nothing in the port raises mTargetFps, so the loop already runs at ~60 fps on
// a 144 Hz display whether VSync is on or off. The premise that a high-refresh
// monitor makes the sim run 2.4x too fast does NOT hold here -- there is no
// speed bug to fix. Following the "preserve correct behaviour" rule, this
// port-level pacer is therefore OPT-IN (default 0) and a strict no-op when off,
// so a stock config behaves byte-for-byte as it does today.
//
// WHEN ENABLED
// ------------
// This enforces the true N64 NTSC field rate (60 / 1.001 ~= 59.94 Hz), which is
// marginally slower than the built-in 60.00 Hz limiter, so THIS pacer becomes
// the binding constraint and the built-in one turns into a no-op for the frame
// (its deadline has already passed). The two therefore do not fight in the
// VSync-off case; the slower schedule simply wins.
//
// VSYNC INTERACTION -- keep VSync OFF when this is ON.
// With VSync on, EndFrame()'s Present() also blocks on the display refresh. On a
// non-60 Hz panel that rounds each present onto the refresh grid (e.g. 2.4
// refreshes per frame at 144 Hz), which beats against this fixed 59.94 Hz
// schedule and produces judder. That judder is a property of VSync at a
// non-integer-multiple refresh and exists for 60 fps content regardless of this
// module; stacking a wall-clock pacer on top only makes it more visible. The
// recommended configuration for this pacer is VSync OFF (tear-free is then the
// built-in limiter's job, which still runs).
//
// Timing primitives: QueryPerformanceCounter/Frequency for the deadline, a
// high-resolution waitable timer for the coarse sleep, and YieldProcessor() for
// the final spin. timeBeginPeriod() is deliberately NOT used so no winmm link
// dependency is added; the high-resolution timer plus the spin backstop already
// hit the deadline accurately, matching libultraship's own approach.

#include "gdx_frame_pacer.h"

#include <windows.h>
#include <stdint.h>

#include "port_log.h" // gdx_port_logf (single diagnostic line on first arm)

// libultraship consolevariablebridge.h is extern "C"/API_EXPORT. Declared locally
// here -- the same minimal-include boundary idiom port/input_bridge.c uses -- so
// this C TU does not pull the C++ bridge header.
extern int CVarGetInteger(const char* name, int defaultValue);

// Target: N64 NTSC field rate = 60 / 1.001 Hz. Frame interval = 1.001 / 60 s.
// Expressed as a rational scale of the QPC frequency: ticks = freq * 1001 / 60000.
#define GDX_PACER_INTERVAL_NUM 1001
#define GDX_PACER_INTERVAL_DEN 60000

// If we fall more than this many whole frames behind the schedule (a hitch: menu
// stall, window drag, alt-tab, debugger breakpoint), re-anchor to "now" instead
// of replaying the missed frames. This clamps catch-up bursts.
#define GDX_PACER_MAX_LAG_FRAMES 4

// Some SDKs predate the high-resolution waitable-timer flag (Win10 1803+).
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static int sInitDone = 0;     // one-time init guard
static int sUsable = 0;       // 0 if QPC unavailable -> pacer degrades to a no-op
static LONGLONG sFreq = 0;    // QPC frequency (ticks/second)
static LONGLONG sIntervalTicks = 0;   // one frame in QPC ticks
static LONGLONG sSpinMarginTicks = 0; // coarse-sleep undershoot margin (~1.5 ms)
static LONGLONG sNextDeadline = 0;    // absolute QPC target for the next boundary; 0 = unarmed
static HANDLE sTimer = NULL;          // waitable timer for the coarse sleep

static void gdx_frame_pacer_init(void) {
    LARGE_INTEGER freq;
    sInitDone = 1;

    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        sUsable = 0; // no monotonic clock -> never pace
        return;
    }
    sFreq = freq.QuadPart;
    sIntervalTicks = sFreq * GDX_PACER_INTERVAL_NUM / GDX_PACER_INTERVAL_DEN;
    if (sIntervalTicks <= 0) {
        sUsable = 0;
        return;
    }
    // ~1.5 ms expressed in QPC ticks (freq * 0.0015 = freq * 3 / 2000).
    sSpinMarginTicks = sFreq * 3 / 2000;

    // Prefer a high-resolution auto-reset timer; fall back to a normal one.
    sTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (sTimer == NULL) {
        sTimer = CreateWaitableTimerW(NULL, FALSE, NULL); // auto-reset
    }
    // sTimer may legitimately be NULL here; the tick path then relies on the spin
    // loop alone (correct, just busier). Not a fatal condition.

    sUsable = 1;
}

// Convert QPC ticks to the 100 ns units SetWaitableTimer expects.
static LONGLONG gdx_ticks_to_100ns(LONGLONG ticks) {
    // ticks is at most a few frame intervals (~1.7e5 at 10 MHz), so ticks * 1e7
    // stays well within int64 range.
    return ticks * 10000000LL / sFreq;
}

void gdx_frame_pacer_tick(void) {
    LARGE_INTEGER now;
    LONGLONG remaining;

    // Live read every frame so the menu toggle takes effect immediately. Default 0.
    if (CVarGetInteger("gEnhancements.Graphics.FramePacing", 0) == 0) {
        // Disabled: strict no-op. Unarm the schedule so a later enable starts from a
        // fresh baseline rather than firing a burst of catch-up frames.
        sNextDeadline = 0;
        return;
    }

    if (!sInitDone) {
        gdx_frame_pacer_init();
    }
    if (!sUsable) {
        return; // no usable clock -> behave like OFF
    }

    QueryPerformanceCounter(&now);

    if (sNextDeadline == 0) {
        // First paced frame (fresh, or first after a re-enable): set the baseline and
        // do not sleep. This handles the "first frame" case with no special-casing of
        // an uninitialised previous timestamp.
        sNextDeadline = now.QuadPart + sIntervalTicks;
        gdx_port_logf("[pacer] FramePacing ON: target ~59.94 Hz (N64 NTSC 60/1.001), "
                      "interval %lld QPC ticks. Recommend VSync OFF.\n",
                      (long long)sIntervalTicks);
        return;
    }

    remaining = sNextDeadline - now.QuadPart;

    // Big stall: we are many frames behind (hitch/pause). Re-anchor; do not catch up.
    if (remaining < -(sIntervalTicks * GDX_PACER_MAX_LAG_FRAMES)) {
        sNextDeadline = now.QuadPart + sIntervalTicks;
        return;
    }

    // At or slightly past the deadline: this frame's own work already spent the
    // budget, so do not sleep. Advance the schedule by whole intervals until it is
    // back in the future, keeping the long-run average locked to the target rate.
    if (remaining <= 0) {
        do {
            sNextDeadline += sIntervalTicks;
        } while (sNextDeadline <= now.QuadPart);
        return;
    }

    // Normal case: wait to the deadline. Coarse sleep to ~1.5 ms short, then spin.
    {
        LONGLONG sleep_ticks = remaining - sSpinMarginTicks;
        if (sleep_ticks > 0 && sTimer != NULL) {
            LARGE_INTEGER due;
            due.QuadPart = -gdx_ticks_to_100ns(sleep_ticks); // negative = relative
            if (SetWaitableTimer(sTimer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(sTimer, INFINITE);
            }
        }
        // Spin the remainder to the exact deadline (also covers a coarse-sleep
        // overshoot, in which case the loop exits immediately).
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= sNextDeadline) {
                break;
            }
            YieldProcessor();
        }
    }

    // Schedule the next boundary from the absolute schedule (not from "now"), so
    // per-frame jitter averages out over time.
    sNextDeadline += sIntervalTicks;

    // Guard against unbounded drift if a spin somehow overshot by many frames.
    if (sNextDeadline < now.QuadPart - sIntervalTicks * GDX_PACER_MAX_LAG_FRAMES) {
        sNextDeadline = now.QuadPart + sIntervalTicks;
    }
}
