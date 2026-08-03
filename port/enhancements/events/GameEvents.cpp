/**
 * @file port/enhancements/events/GameEvents.cpp
 * @brief The one translation unit that OWNS the EventID storage, plus the fire helpers the
 *        PORT-gated decomp one-liners call.
 *
 * See GameEvents.h for the architecture rationale. This file is deliberately thin: it holds no
 * enhancement policy of its own, only the plumbing that lets policy live in port/enhancements/.
 */

/* MUST precede the GameEvents.h include and MUST NOT be defined in any other translation unit.
   It flips DECLARE_EVENT (libultraship/include/ship/events/EventTypes.h:78-88) from "extern
   declaration" to "definition initialised to -1", so this .cpp is where `OnBoostStartID` actually
   lives. Defining it twice would be a duplicate-symbol link error; defining it nowhere would be an
   unresolved external. */
#define INIT_EVENT_IDS
#include "GameEvents.h"

#include "port_log.h"

// =================================================================================================
// Installer registry
// =================================================================================================
// Enhancement modules cannot subscribe at static-initialisation time: REGISTER_LISTENER routes
// through Ship::Context::GetInstance() (eventsbridge.cpp:13) and the Context does not exist until
// port/main.cpp:1033 runs InitEventSystem(). So they hand us a function pointer during static init
// and we call it back once the IDs are live.
//
// Storage is a plain fixed array rather than std::vector on purpose. A namespace-scope array of
// pointers is CONSTANT-initialised (zeroed) before any dynamic initialiser anywhere in the program
// executes, so GameEvents_AddInstaller is safe to call from another TU's static constructor
// regardless of link order. A std::vector would itself need dynamic initialisation and could
// legally be constructed *after* the first caller — the classic static-init-order fiasco.
static constexpr int kMaxInstallers = 32;
static GameEventsInstaller sInstallers[kMaxInstallers];
static int sInstallerCount = 0;

// Set once GameEvents_Init has registered the event IDs. Guards against the re-registration hazard
// documented on GameEvents_Init in the header (EventSystemRegisterEvent hands out a new ID every
// call), and tells GameEvents_AddInstaller whether it must run a late arrival immediately.
static bool sInitialised = false;

// Not atomic, and that is correct here rather than lazy: everything that touches this state runs on
// the single cooperatively-scheduled thread that owns both the decomp game fibers and the port
// glue (the same invariant port/gdx_interp.cpp relies on for its cut epoch), and installers only
// arrive during single-threaded static initialisation before main().

void GameEvents_AddInstaller(GameEventsInstaller installer) {
    if (installer == nullptr) {
        return;
    }

    if (sInitialised) {
        // Late arrival: the bus is already up, so there is nothing to defer — subscribe now.
        // Cannot happen for a statically-linked enhancement (all static init completes before
        // main), but a future dynamically-loaded module would land here.
        installer();
        return;
    }

    if (sInstallerCount >= kMaxInstallers) {
        // Deliberately loud rather than silently dropping an enhancement the user enabled. Raising
        // kMaxInstallers is the fix; there is no reason to grow this dynamically.
        gdx_port_logf("[events] installer registry full (%d); enhancement dropped\n", kMaxInstallers);
        return;
    }

    sInstallers[sInstallerCount++] = installer;
}

void GameEvents_Init(void) {
    if (sInitialised) {
        return;
    }
    sInitialised = true;

    // REGISTER_EVENT assigns OnBoostStartID. Until this runs the ID is -1, which is why the fire
    // helpers below call GameEvents_Init first: EventSystem::RegisterListener THROWS on id == -1
    // (EventSystem.cpp:17-19), so a listener that beat registration would take down the process.
    REGISTER_EVENT(OnBoostStart);

    for (int i = 0; i < sInstallerCount; i++) {
        sInstallers[i]();
    }

    gdx_port_logf("[events] registered 1 event, %d enhancement installer(s)\n", sInstallerCount);
}

// =================================================================================================
// Fire helpers — the C-linkage entry points the PORT-gated decomp one-liners call
// =================================================================================================

void GameEvents_FireOnBoostStart(int32_t racerId, int32_t* frames) {
    // Self-initialise on first fire. This is what keeps the whole layer out of port/main.cpp: the
    // first boost of a session can only happen long after Ship::Context is built, so the bridge's
    // Ship::Context::GetInstance() dereference is always safe by the time we get here. Calling
    // GameEvents_Init() explicitly from startup instead is equally valid — it is idempotent.
    GameEvents_Init();

    // Defensive: the current fire site always passes &racer->boostTimer, but a listener
    // dereferencing null would be a crash in third-party .o2r mod code with no useful backtrace.
    if (frames == nullptr) {
        return;
    }

    // Positional aggregate init, NOT the designated-initialiser form shown in
    // libultraship/docs/SCRIPTING.md:319 — CALL_EVENT already supplies the `{ false }` IEvent base
    // positionally, and C++20 forbids mixing positional and designated initialisers in one braced
    // list. The macro expands to a stack payload; listeners mutate it through the pointer field, so
    // the write lands directly on the game's own `Racer::boostTimer`.
    CALL_EVENT(OnBoostStart, racerId, frames);
}
