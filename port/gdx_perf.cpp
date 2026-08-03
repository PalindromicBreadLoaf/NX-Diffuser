// G-Diffuser — lightweight frame-time telemetry. See gdx_perf.h for the contract.

#include "gdx_perf.h"
#include "gdx_dev_gates.h" // GDX_PERF is now a Dev Tools gate (Bucket A), re-latched per frame
#include "port_log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace gdx {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kSpikeThresholdMs = 25.0; // ~1.5 dropped 60Hz frames — worth an immediate line
constexpr int kSummaryWindowFrames = 600;  // ~10 s at 60 fps

const char* const kPhaseNames[PerfPhaseCount] = {
    "events", "input", "gametick", "guistart", "dispatch", "ticks", "present", "pacer",
};

struct PerfState {
    bool enabled = false;

    Clock::time_point frameStart{};
    Clock::time_point phaseStart[PerfPhaseCount]{};
    double phaseMs[PerfPhaseCount]{};       // current frame
    double phaseAccumMs[PerfPhaseCount]{};  // summary window accumulation

    // Second-level breakdown inside the gametick phase (main-thread-only; no lock needed).
    Clock::time_point subStart[GDX_PERF_SUB_COUNT]{};
    double subMs[GDX_PERF_SUB_COUNT]{};      // current frame
    double subAccumMs[GDX_PERF_SUB_COUNT]{}; // summary window accumulation
    std::thread::id mainThread{};            // captured at PerfFrameBegin; sub timers guard against it

    std::vector<double> frameTotals; // summary window frame totals (ms)
    int spikeCount = 0;              // spikes within the current window

    // Audio thread tick durations for the current window (guarded by mtx; ~200 Hz writer).
    std::mutex mtx;
    std::vector<double> audioTicks;
};

// Derived "game logic" time: the gametick fiber run minus the gfx-submission sub-work it contains.
// Clamped at 0 in case a seam's wall-clock straddles the phase boundary by a hair.
double subLogicMs(double gametickMs, const double* sub) {
    double logic = gametickMs - sub[GDX_PERF_SUB_XLATE] - sub[GDX_PERF_SUB_RUN] - sub[GDX_PERF_SUB_MIRROR];
    return logic > 0.0 ? logic : 0.0;
}

PerfState& state() {
    static PerfState s; // holds a mutex — must be constructed in place, never copied
    static const bool initialized = [] {
        // Reserve unconditionally (a few tens of KB) so that enabling telemetry from the Dev Tools
        // menu mid-session never allocates on the frame loop's first measured frame. `enabled`
        // itself is re-latched once per frame in PerfFrameBegin from the gate cache.
        s.frameTotals.reserve(kSummaryWindowFrames);
        s.audioTicks.reserve(2048);
        return true;
    }();
    (void) initialized;
    return s;
}

double toMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// Percentile over a scratch copy (summary path only — never per frame).
double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

void emitSummary(PerfState& s) {
    std::vector<double> totals = s.frameTotals;
    std::sort(totals.begin(), totals.end());
    const double p50 = percentile(totals, 0.50);
    const double p95 = percentile(totals, 0.95);
    const double p99 = percentile(totals, 0.99);
    const double mx = totals.empty() ? 0.0 : totals.back();

    const double n = totals.empty() ? 1.0 : static_cast<double>(totals.size());
    char phases[256];
    size_t off = 0;
    for (int i = 0; i < PerfPhaseCount; ++i) {
        off += static_cast<size_t>(std::snprintf(phases + off, sizeof(phases) - off, "%s=%.2f ",
                                                 kPhaseNames[i], s.phaseAccumMs[i] / n));
        if (off >= sizeof(phases)) {
            break;
        }
    }

    double aP95 = 0.0, aMax = 0.0;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        std::vector<double> at = s.audioTicks;
        s.audioTicks.clear();
        std::sort(at.begin(), at.end());
        aP95 = percentile(at, 0.95);
        aMax = at.empty() ? 0.0 : at.back();
    }

    // Window-mean sub-phase breakdown of the gametick phase (logic derived by subtraction).
    double subMean[GDX_PERF_SUB_COUNT];
    for (int i = 0; i < GDX_PERF_SUB_COUNT; ++i) {
        subMean[i] = s.subAccumMs[i] / n;
    }
    const double logicMean = subLogicMs(s.phaseAccumMs[PerfGameTick] / n, subMean);

    gdx_port_logf("[GDX perf] summary frames=%zu p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms "
                  "spikes=%d | mean: %s| sub: logic=%.2f xlate=%.2f run=%.2f mirror=%.2f "
                  "| audio p95=%.2fms max=%.2fms\n",
                  totals.size(), p50, p95, p99, mx, s.spikeCount, phases, logicMean,
                  subMean[GDX_PERF_SUB_XLATE], subMean[GDX_PERF_SUB_RUN], subMean[GDX_PERF_SUB_MIRROR],
                  aP95, aMax);

    s.frameTotals.clear();
    s.spikeCount = 0;
    std::memset(s.phaseAccumMs, 0, sizeof(s.phaseAccumMs));
    std::memset(s.subAccumMs, 0, sizeof(s.subAccumMs));
}

} // namespace

bool PerfEnabled() {
    return state().enabled;
}

void PerfFrameBegin() {
    PerfState& s = state();
    // Re-latch the gate ONCE per frame, here, before any PerfPhaseBegin runs. Every other entry
    // point below tests the latched flag, so a toggle mid-frame can never leave a Begin without its
    // End (or vice versa) — the whole frame is measured or none of it is.
    const bool wasEnabled = s.enabled;
    s.enabled = gdx_dev_gate(GDX_GATE_PERF) != 0;
    if (s.enabled && !wasEnabled) {
        gdx_port_logf("[GDX perf] telemetry enabled (spike threshold %.0f ms, summary every %d frames)\n",
                      kSpikeThresholdMs, kSummaryWindowFrames);
    }
    if (!s.enabled) {
        return;
    }
    s.frameStart = Clock::now();
    s.mainThread = std::this_thread::get_id(); // the frame loop always runs on the host/main thread
    std::memset(s.phaseMs, 0, sizeof(s.phaseMs));
    std::memset(s.subMs, 0, sizeof(s.subMs));
}

void PerfPhaseBegin(PerfPhase p) {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    s.phaseStart[p] = Clock::now();
}

void PerfPhaseEnd(PerfPhase p) {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    s.phaseMs[p] += toMs(Clock::now() - s.phaseStart[p]);
}

void PerfFrameEnd() {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    const double total = toMs(Clock::now() - s.frameStart);
    for (int i = 0; i < PerfPhaseCount; ++i) {
        s.phaseAccumMs[i] += s.phaseMs[i];
    }
    for (int i = 0; i < GDX_PERF_SUB_COUNT; ++i) {
        s.subAccumMs[i] += s.subMs[i];
    }
    s.frameTotals.push_back(total);

    // The pacer/present phases legitimately wait for vsync — a "spike" only matters when the
    // WORK phases blow the budget, so subtract the deliberate waits from the spike test while
    // still reporting them in the breakdown.
    const double waitMs = s.phaseMs[PerfPresent] + s.phaseMs[PerfPacer];
    const double workMs = total - waitMs;
    if (workMs > kSpikeThresholdMs) {
        ++s.spikeCount;
        const double logic = subLogicMs(s.phaseMs[PerfGameTick], s.subMs);
        gdx_port_logf("[GDX perf] SPIKE work=%.1fms total=%.1fms: events=%.1f input=%.1f "
                      "gametick=%.1f guistart=%.1f dispatch=%.1f ticks=%.1f present=%.1f pacer=%.1f "
                      "| sub: logic=%.1f xlate=%.1f run=%.1f mirror=%.1f\n",
                      workMs, total, s.phaseMs[PerfEvents], s.phaseMs[PerfInput],
                      s.phaseMs[PerfGameTick], s.phaseMs[PerfGuiStart], s.phaseMs[PerfDispatch],
                      s.phaseMs[PerfTicks], s.phaseMs[PerfPresent], s.phaseMs[PerfPacer],
                      logic, s.subMs[GDX_PERF_SUB_XLATE], s.subMs[GDX_PERF_SUB_RUN],
                      s.subMs[GDX_PERF_SUB_MIRROR]);
    }

    if (s.frameTotals.size() >= static_cast<size_t>(kSummaryWindowFrames)) {
        emitSummary(s);
    }
}

void PerfAudioTick(double ms) {
    PerfState& s = state();
    if (!s.enabled) {
        return;
    }
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.audioTicks.size() < 100000) { // hard cap; cleared every summary window
        s.audioTicks.push_back(ms);
    }
}

} // namespace gdx

// C-callable sub-phase seams (see gdx_perf.h). Defined with C linkage so the C++ gfx bridge and any
// C translation unit share them. Zero cost when disabled (cached-bool early return, like the phase
// API). Main-thread guard: the game fiber (and thus gdx_gfx_run's seams) run on the host thread; a
// stray off-thread call is skipped rather than allowed to race the unsynchronized sub timers.
extern "C" void gdx_perf_sub_begin(int id) {
    gdx::PerfState& s = gdx::state();
    if (!s.enabled || id < 0 || id >= GDX_PERF_SUB_COUNT) {
        return;
    }
    if (s.mainThread != std::this_thread::get_id()) {
        return;
    }
    s.subStart[id] = gdx::Clock::now();
}

extern "C" void gdx_perf_sub_end(int id) {
    gdx::PerfState& s = gdx::state();
    if (!s.enabled || id < 0 || id >= GDX_PERF_SUB_COUNT) {
        return;
    }
    if (s.mainThread != std::this_thread::get_id()) {
        return;
    }
    s.subMs[id] += gdx::toMs(gdx::Clock::now() - s.subStart[id]);
}
