/**
 * @file port/enhancements/tuning/BoostDuration.cpp
 * @brief Enhancement: override how many frames a boost lasts. First consumer of the
 *        port/enhancements event layer, and the reference example for writing another.
 *
 * WHAT IT DEMONSTRATES
 * --------------------
 * This file changes a gameplay value that lives inside the matching decompilation, yet it contains
 * no decomp diff of its own and nothing central had to be told it exists. Its whole contract with
 * the rest of the program is:
 *   1. a file-scope static initialiser that hands GameEvents_AddInstaller a callback, and
 *   2. one entry in port/CMakeLists.txt's add_executable(G-Diffuser ...) list.
 * That is the pattern every future enhancement should copy. See port/enhancements/events/
 * GameEvents.h for why the event bus exists at all.
 *
 * CVAR
 * ----
 *   gEnhancements.Tuning.BoostDuration  (int, default 0)
 *     0 or below -> stock behaviour; the listener writes nothing at all.
 *     n > 0      -> a boost lasts n frames instead of the stock 100 (decomp/src/game/racer.c:387).
 *
 * There is deliberately NO menu UI yet. The event layer is the thing being proven here; wiring a
 * slider into port/gdx_menu.cpp is a separate, trivial follow-up and would have coupled this change
 * to a file other work is actively editing. Set the CVar from LUS's console
 * (`set gEnhancements.Tuning.BoostDuration 300`) or from the shipped CVar config file.
 *
 * SCOPE NOTE: only the manual BTN_B boost is hooked (racer.c:3623). The dash-pad boost 18 lines
 * later (racer.c:3641) arms the same `boostTimer` from the same `sInitialBoostTimer` and is
 * intentionally left stock — hooking it is a one-line follow-up once this seam is proven, and
 * keeping the first fire site singular makes it unambiguous which code path a behaviour change
 * came from during testing.
 */

#include "enhancements/events/GameEvents.h"

#include "libultraship/bridge/consolevariablebridge.h"

namespace {

// Upper clamp. `boostTimer` is an s32 counted down each tick and is also the DIVISOR at
// racer.c:4460 (rumble strength) and racer.c:6093 (boost-flame scale) — those divide by
// `sInitialBoostTimer`, not by this value, so an absurd override cannot produce a divide-by-zero,
// but it can produce a boost that outlives the race. 100000 frames is ~28 minutes at 60Hz: far
// past any plausible use, comfortably inside s32, and a clear signal that a fat-fingered CVar was
// clamped rather than silently accepted.
constexpr int32_t kMaxBoostFrames = 100000;

void OnBoostStartListener(IEvent* event) {
    // reinterpret_cast, not static_cast: DEFINE_EVENT (EventTypes.h:107) EMBEDS IEvent as the first
    // member rather than deriving from it, so the two types are formally unrelated and static_cast
    // is ill-formed. Both structs are standard-layout and IEvent is the first member, so a pointer
    // to the payload and a pointer to its IEvent subobject are interconvertible ([basic.compound]).
    // This is the cast the engine's own documentation prescribes (SCRIPTING.md:345, in C-cast
    // spelling); reinterpret_cast just says out loud what that C cast was already doing.
    auto* e = reinterpret_cast<OnBoostStart*>(event);

    // Deliberately NOT filtered by `e->racerId`: Racer_UpdateFromControls runs for every machine on
    // the grid, CPU racers included, so this knob changes boost duration for the whole field rather
    // than handing the human player an advantage. That keeps it a *tuning* value (a changed rule of
    // the game) instead of a cheat, and it is the behaviour that composes sensibly with the ghost
    // and replay systems. An enhancement that genuinely wants to be player-only checks
    // `e->racerId < gNumPlayers` — see GameEvents.h's note on the racerId payload field.
    const int32_t configured = CVarGetInteger("gEnhancements.Tuning.BoostDuration", 0);

    // 0 is "stock", and so is any negative value. Negative is not merely meaningless, it is
    // dangerous: the surrounding decomp branch re-arms a boost only when `racer->boostTimer == 0`
    // (racer.c:3618), and the countdown decrements past a negative start value without ever
    // hitting zero — the racer would be locked out of boosting for the rest of the race. Treat
    // anything <= 0 as "do not touch the value".
    if (configured <= 0) {
        return;
    }

    // Writing through the payload pointer mutates the caller's own `Racer::boostTimer`; see
    // GameEvents.h for why EventSystem::CallEvent makes that legal.
    *e->frames = (configured > kMaxBoostFrames) ? kMaxBoostFrames : configured;
}

void Install() {
    // EVENT_PRIORITY_NORMAL, not HIGH. The engine's dispatch order is the reverse of its
    // documentation (see the priority note in GameEvents.h): listeners run LOW -> NORMAL -> HIGH,
    // so HIGH would make this the LAST writer and let it stomp any future enhancement. NORMAL
    // keeps this well-behaved as a baseline that something more specific can still override.
    //
    // The ListenerID is intentionally dropped. This enhancement is statically linked into the
    // executable and lives for the whole process, so there is no unload path that could leave the
    // EventSystem holding a dangling callback — unlike an .o2r script mod, which must keep its ID
    // and unregister in MOD_EXIT (libultraship/docs/SCRIPTING.md, "Registering and Unregistering
    // Listeners").
    REGISTER_LISTENER(OnBoostStart, EVENT_PRIORITY_NORMAL, OnBoostStartListener);
}

// Self-installation. Runs during static initialisation, before main() and therefore before
// GameEvents_Init, so the callback is queued rather than invoked (GameEvents.cpp's registry is a
// constant-initialised array precisely so this is safe at any link order).
//
// `const bool` at namespace scope has internal linkage, so this cannot collide with an identically
// shaped line in another enhancement file. The object exists only for its initialiser's side
// effect; [[maybe_unused]] documents that and silences -Wunused-variable.
[[maybe_unused]] const bool sInstalled = (GameEvents_AddInstaller(&Install), true);

} // namespace
