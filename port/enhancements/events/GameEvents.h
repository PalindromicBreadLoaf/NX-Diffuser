/**
 * @file port/enhancements/events/GameEvents.h
 * @brief G-Diffuser gameplay event surface — the seam between the matching decomp and port/.
 *
 * WHY THIS EXISTS
 * ---------------
 * decomp/ is a *matching* decompilation: every non-PORT build must stay byte-identical to the
 * retail ROM, so enhancement logic cannot live there. Up to now every port-side behaviour change
 * has been an `#ifdef PORT` island inside decomp/ (see decomp/src/game/racer.c:763, :1703, :5587
 * for the frame-interpolation cut shims). That scales badly: each new enhancement means another
 * decomp diff to rebase on top of upstream inspectredc/fzerox.
 *
 * This header inverts that. decomp/ gains exactly ONE `#ifdef PORT` one-liner per hook point,
 * which does nothing but announce "this happened, here is the value I am about to use". All
 * actual enhancement logic lives in port/enhancements/ as a listener. Adding an enhancement then
 * costs zero further decomp churn.
 *
 * WHICH ENGINE FACILITY THIS USES
 * -------------------------------
 * libultraship already ships an unused publish/subscribe bus (libultraship/src/ship/events/
 * EventSystem.cpp, C entry points in libultraship/src/libultraship/bridge/eventsbridge.cpp).
 * We use it as-is rather than inventing a port-local callback list, because:
 *   - EventSystem::CallEvent (EventSystem.cpp:55) hands every listener a NON-const `IEvent*` that
 *     points straight at the caller's stack payload. A listener can therefore write through a
 *     pointer field and change a live game value — which is exactly what an enhancement needs.
 *   - It is already reachable from C (eventsbridge.h), already inspectable at runtime (LUS's
 *     EventDebuggerWindow, libultraship/src/ship/window/gui/EventDebuggerWindow.cpp:83 walks the
 *     registry and shows per-call-site hit counts), and already exported for .o2r script mods
 *     (libultraship/docs/SCRIPTING.md, "Event System"), so third-party mods get these hooks free.
 *
 * TRANSLATION-UNIT CONTRACT
 * -------------------------
 * DEFINE_EVENT (libultraship/include/ship/events/EventTypes.h:107) emits the payload struct plus
 * DECLARE_EVENT, whose meaning flips on INIT_EVENT_IDS:
 *   - INIT_EVENT_IDS defined  -> `extern "C" EventID OnBoostStartID = -1;`  (a DEFINITION)
 *   - INIT_EVENT_IDS undefined-> `API_EXPORT EventID OnBoostStartID;`       (a DECLARATION; per
 *     [dcl.link]/7 a declaration directly inside a linkage-specification is treated as `extern`)
 * So exactly ONE translation unit — GameEvents.cpp — may `#define INIT_EVENT_IDS` before including
 * this header. Every other includer gets plain declarations and links against those definitions.
 *
 * LISTENER PRIORITY — READ THIS BEFORE PICKING ONE
 * ------------------------------------------------
 * The engine's documented contract (EventTypes.h:12-20, SCRIPTING.md "Event Priority") says
 * EVENT_PRIORITY_HIGH listeners run FIRST. The implementation does the opposite:
 * EventSystem::RegisterListener (EventSystem.cpp:24-29) inserts with a `std::lower_bound` whose
 * comparator is `existing.Priority < toInsert.Priority`, which keeps the vector sorted ASCENDING,
 * and CallEvent iterates front-to-back. Real dispatch order is therefore LOW -> NORMAL -> HIGH.
 * For a mutable payload that means the HIGHEST priority listener writes LAST and wins. Until that
 * is fixed upstream, do not rely on priority for ordering; assume "last writer wins" and prefer
 * EVENT_PRIORITY_NORMAL unless you have a concrete reason.
 */

#pragma once

#include <stdint.h>

#ifndef __cplusplus
/* IEvent::Cancelled (EventTypes.h:32) is a `bool`, but EventTypes.h only pulls <stdint.h>. Keep
   this header includable from a C translation unit even though nothing does so today — the decomp
   side deliberately reaches the fire helpers through a local `extern` declaration instead (see the
   "DECOMP SIDE" note below), so C includers are a courtesy, not a requirement. */
#include <stdbool.h>
#endif

/* eventsbridge.h rather than ship/events/EventTypes.h directly: EventTypes.h supplies only the
   DEFINE_EVENT / REGISTER_EVENT / CALL_EVENT / REGISTER_LISTENER macros, while the C functions
   those macros expand to (EventSystemRegisterEvent, EventSystemCallEvent, ...) are declared in the
   bridge. Pulling the bridge here means an enhancement .cpp gets the complete surface from this
   one include, the way ship/events/CoreEvents.h does for engine-side code. */
#include "libultraship/bridge/eventsbridge.h"

/**
 * @brief Fired the instant a racer's boost timer is (re)armed, before the game uses the value.
 *
 * @param racerId `Racer::id` (decomp/include/unk_structs.h:170) of the machine that boosted.
 *                0..gNumPlayers-1 are human players; higher ids are CPU racers. A listener that
 *                only wants to affect the local player must check this — the fire site runs for
 *                every racer on the grid, CPU included.
 * @param frames  Pointer to the live `Racer::boostTimer` (unk_structs.h:232, `s32`) the game just
 *                wrote. A listener may overwrite `*frames` to change how long the boost lasts.
 *                The pointer is never null at the current fire site.
 *
 * Stock value is `sInitialBoostTimer` = 100 frames (decomp/src/game/racer.c:387), which the
 * TASVideos F-Zero X documentation independently corroborates ("both boost methods give 100
 * frames"). Note that 100 is not just a duration: racer.c:4455 compares `boostTimer` against
 * `sInitialBoostTimer - 1` and racer.c:4460/:6093 divide by `sInitialBoostTimer` to drive the
 * rumble strength and the boost-flame scale. Those all read the *variable*, not the literal, so
 * a listener that rewrites only `*frames` leaves them normalised against 100 — a longer boost
 * therefore keeps its visual/rumble ramp on the stock curve rather than stretching it. That is a
 * deliberate, acceptable cosmetic mismatch for a tuning knob, not an oversight.
 *
 * NOT cancellable. The fire site uses CALL_EVENT, not CALL_CANCELLABLE_EVENT, because "cancel the
 * boost" is not a meaningful operation here: by the time we fire, the sound-effect flag, shadow
 * colour and energy gating have already been committed by the surrounding decomp branch. Setting
 * `*frames = 1` is the closest honest equivalent and needs no cancellation plumbing.
 */
DEFINE_EVENT(OnBoostStart,
             int32_t racerId;
             int32_t* frames;)

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Signature of an enhancement's self-installation hook (see GameEvents_AddInstaller). */
typedef void (*GameEventsInstaller)(void);

/**
 * @brief Registers every event type with LUS's EventSystem, then runs all pending installers.
 *
 * Idempotent and cheap to call repeatedly — this matters because EventSystemRegisterEvent
 * (eventsbridge.cpp:7 -> EventSystem.cpp:9) unconditionally allocates a FRESH EventID per call
 * despite its doc comment promising name-based deduplication. A second unguarded registration
 * would therefore orphan every listener already attached to the old ID.
 *
 * Safe to call from anywhere once Ship::Context exists (the bridge dereferences
 * Ship::Context::GetInstance()->GetEventSystem(), eventsbridge.cpp:8). Callers do not have to
 * call it at all: the fire helpers below self-initialise on first use, which is why this layer
 * needs no edit to port/main.cpp.
 */
void GameEvents_Init(void);

/**
 * @brief Enqueues @p installer to be run by GameEvents_Init once the event IDs exist.
 *
 * Enhancement modules call this from a file-scope static initialiser, so dropping a new .cpp into
 * the build is all it takes to add an enhancement — nothing central has to learn its name. The
 * backing storage is a zero-initialised static array, which is constant-initialised before ANY
 * dynamic initialiser in the program runs, so this is immune to static-initialisation-order
 * fiasco no matter which object file the linker orders first.
 *
 * Installers registered after GameEvents_Init has already run are executed immediately, so a
 * lazily-loaded module cannot silently miss its window.
 */
void GameEvents_AddInstaller(GameEventsInstaller installer);

/**
 * @brief Fires OnBoostStart. Called from the PORT-gated one-liner in decomp/src/game/racer.c.
 *
 * DECOMP SIDE: the gdiffuser_game target compiles decomp/ with only the decomp include paths
 * (port/CMakeLists.txt:214-219) — it has no libultraship headers — so racer.c cannot include this
 * file. It declares this prototype inline at the call site instead, matching the existing shim
 * idiom at racer.c:1703. `s32` is `signed int` (decomp/include/common.h:6) and `int32_t` is `int`
 * on every toolchain this port builds with, so the two spellings are the same type and the C
 * linkage name matches exactly.
 */
void GameEvents_FireOnBoostStart(int32_t racerId, int32_t* frames);

#ifdef __cplusplus
}
#endif
