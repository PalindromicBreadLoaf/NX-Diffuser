// port/gdx_savestate.c -- in-session (RAM) quick-save / quick-load, curated same-race rewind.
//
// ==============================================================================================
// WHAT THIS IS
// ==============================================================================================
// A real in-session rewind for an active race. The snapshot slot has two parts:
//   1. The full 16 MB emulated RDRAM buffer (gdx_rdram) -- RCP-visible data: GfxPool display-list
//      scratch, Arena_Allocate'd course geometry/models/textures, the persistent audio soundfont/
//      sequence heap, and ROM DMA targets.
//   2. A CURATED set of the game's race-critical NATIVE globals. G-Diffuser is a static
//      decompilation compiled to native code: the moment-to-moment simulation state (machine
//      physics, racer logical state, cameras, RNG, race-progress counters, game-mode flow) lives
//      in the host EXE's .data/.bss as native C globals, NOT inside gdx_rdram. So an RDRAM-only
//      copy alone does not rewind the cars; the curated block is what makes the rewind faithful.
//
// The curated globals are captured/restored through tiny additive accessors compiled into the
// owning decomp translation units under #ifdef PORT (they have the type knowledge and can reach
// even the file-static globals):
//   - decomp/src/game/game_context.c : gRacers[30]                          (Gdx_SaveState_GameContext_*)
//   - decomp/src/game/racer.c        : gMachines[30] + the race-progress/ghost/counter block
//                                                                            (Gdx_SaveState_Racer_*)
//   - decomp/src/game/camera.c       : gCameras[4] + camera settings/scripts (Gdx_SaveState_Camera_*)
//   - decomp/src/sys/math.c          : gRandSeed1/gRandMask1/gRandSeed2/gRandMask2 (Gdx_SaveState_Rng_*)
//   - decomp/src/game/game.c         : gGameMode + race-flow scalars         (Gdx_SaveState_Game_*)
//
// ==============================================================================================
// POINTER SAFETY + THE SAME-COURSE / SAME-RACE CONSTRAINT (read before enabling)
// ==============================================================================================
// The curated structs contain pointers. They are safe to raw-copy because of WHERE they point:
//   - Pointers into other fixed native BSS globals (e.g. Racer.racerAhead/racerBehind/unk_28C and
//     gRacersByPosition[] -> &gRacers[k]; GhostRacer.ghost -> &gGhosts[k]; CameraScriptManager
//     .racer/.settings -> &gRacers[k]/&sCameraSettings[k]) keep the SAME address across the
//     process, so restoring the raw pointer value is valid and the pointed-to bytes are restored
//     by the same snapshot.
//   - CameraScript.updateFunc is a host FUNCTION pointer -- code does not move within a single
//     process run, so it is invariant between save and load.
//   - Pointers into the RDRAM arena (e.g. Racer.segmentPositionInfo.courseSegment when it targets
//     loaded course-segment data) are valid after restore BECAUSE part 1 restores that RDRAM data
//     too -- but ONLY IF THE ARENA LAYOUT IS IDENTICAL, i.e. THE SAME COURSE IS LOADED, WITH NO
//     COURSE/MODE RELOAD BETWEEN SAVE AND LOAD.
//
//   >>> SUPPORTED SCOPE: quick-load only rewinds faithfully when the same race on the same course
//   >>> is still loaded (the intended "save mid-race, drive, load" use). If the course/mode was
//   >>> reloaded in between, the arena layout differs and restored pointers can target the wrong
//   >>> data. This build does not durably fingerprint the course, so it CANNOT hard-refuse that
//   >>> case; it is the documented, owner-tested constraint for this feature.
//
// EXPLICITLY EXCLUDED (would embed host/engine handles or non-rewindable state):
//   - gAudioCtx.* (audio context): holds host pointers, in-flight DMA state, and handler function
//     pointers -> never captured. Consequence: AUDIO DOES NOT REWIND (music keeps playing from its
//     current position). The RDRAM audio heap IS in part 1, but its soundfont/sequence data is
//     stable within a session, so restoring it is effectively a no-op for a same-course rewind and
//     is done under the audio lock so the audio thread never sees a torn buffer.
//   - The n64_sched.c host bookkeeping (sThreads[] fiber handles, sHostFiber), libultraship device
//     handles, SDL/DX state -> never touched.
//
// ==============================================================================================
// SCHEDULER STATE: NOT CURATED (and why that is correct)
// ==============================================================================================
// gdx_savestate_tick() runs right after gdx_dispatch() returns, where the run queue has drained to
// the tail sentinel: __osRunningThread == NULL and every decomp game fiber is blocked at its
// frame-invariant retrace/message wait. That parked configuration is IDENTICAL every frame, so the
// scheduler bookkeeping (run/active queues, per-OSThread state, the OSMesgQueue the threads block
// on) is already in the exact state a resumed frame expects. A restore does not touch the fibers;
// they simply resume from the same parked point and re-run game logic against the restored RDRAM +
// curated globals. Therefore no scheduler/timer state needs capturing. (F-Zero X race timing is
// driven by sRaceFrameCount, which IS curated, not by osSetTimer.)
//
// ==============================================================================================
// SAFETY / BOUNDARY / DEFAULT-OFF (unchanged from the infrastructure build)
// ==============================================================================================
// Everything is gated behind the CVar gEnhancements.Gameplay.SaveStates (default 0). When 0, this
// module is a STRICT no-op: gdx_savestate_save/load never arm, and gdx_savestate_tick returns
// before any allocation, RDRAM copy, or accessor call (clearing any stale arm flags). No malloc is
// ever reached and no game-visible state is touched unless the CVar is explicitly enabled.
//
// The snapshot/restore both run at the parked boundary while holding gdx_audio_ctx_lock(), which
// serializes against the dedicated audio thread (the only other RDRAM toucher). The game fibers
// are parked, so the curated native globals have no concurrent writer during the copy.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// n64_rdram.h is the single source of truth for the gdx_rdram base pointer and GDX_RDRAM_SIZE.
// Safe to include from this HOST-side TU as long as size_t is already defined (hence <stddef.h>).
#include "n64_rdram.h"

#include "port_log.h" // gdx_port_logf

// libultraship consolevariablebridge.h is extern "C"/API_EXPORT; declared locally -- the same
// minimal-include boundary idiom port/gdx_frame_pacer.c uses -- so this C TU avoids the C++ header.
extern int CVarGetInteger(const char* name, int defaultValue);

// Audio context mutex, exported with C linkage from port/gdx_audio_thread.cpp.
extern void gdx_audio_ctx_lock(void);
extern void gdx_audio_ctx_unlock(void);

// Curated-global accessors, compiled into the decomp TUs under #ifdef PORT. Prototypes use only
// void*/unsigned int so this host TU needs none of the decomp game types.
extern unsigned int Gdx_SaveState_GameContext_Size(void);
extern void         Gdx_SaveState_GameContext_Capture(void* dst);
extern void         Gdx_SaveState_GameContext_Restore(const void* src);
extern unsigned int Gdx_SaveState_Racer_Size(void);
extern void         Gdx_SaveState_Racer_Capture(void* dst);
extern void         Gdx_SaveState_Racer_Restore(const void* src);
extern unsigned int Gdx_SaveState_Camera_Size(void);
extern void         Gdx_SaveState_Camera_Capture(void* dst);
extern void         Gdx_SaveState_Camera_Restore(const void* src);
extern unsigned int Gdx_SaveState_Rng_Size(void);
extern void         Gdx_SaveState_Rng_Capture(void* dst);
extern void         Gdx_SaveState_Rng_Restore(const void* src);
extern unsigned int Gdx_SaveState_Game_Size(void);
extern void         Gdx_SaveState_Game_Capture(void* dst);
extern void         Gdx_SaveState_Game_Restore(const void* src);

#define GDX_SAVESTATE_CVAR "gEnhancements.Gameplay.SaveStates"

typedef unsigned int (*GdxSsSizeFn)(void);
typedef void (*GdxSsCaptureFn)(void*);
typedef void (*GdxSsRestoreFn)(const void*);

typedef struct GdxSsBlock {
    const char*    name;
    GdxSsSizeFn    size;
    GdxSsCaptureFn capture;
    GdxSsRestoreFn restore;
} GdxSsBlock;

// Fixed order; the versioned header records each block's size so a restore can validate the layout.
static const GdxSsBlock sBlocks[] = {
    { "gamecontext", Gdx_SaveState_GameContext_Size, Gdx_SaveState_GameContext_Capture, Gdx_SaveState_GameContext_Restore },
    { "racer",       Gdx_SaveState_Racer_Size,       Gdx_SaveState_Racer_Capture,       Gdx_SaveState_Racer_Restore },
    { "camera",      Gdx_SaveState_Camera_Size,      Gdx_SaveState_Camera_Capture,      Gdx_SaveState_Camera_Restore },
    { "rng",         Gdx_SaveState_Rng_Size,         Gdx_SaveState_Rng_Capture,         Gdx_SaveState_Rng_Restore },
    { "game",        Gdx_SaveState_Game_Size,        Gdx_SaveState_Game_Capture,        Gdx_SaveState_Game_Restore }
};
#define GDX_SS_BLOCK_COUNT ((unsigned int)(sizeof(sBlocks) / sizeof(sBlocks[0])))

#define GDX_SS_MAGIC   0x47445353u /* 'G','D','S','S' */
#define GDX_SS_VERSION 2u          /* v1 = RDRAM only; v2 = RDRAM + curated globals */

typedef struct GdxSaveStateHeader {
    unsigned int magic;
    unsigned int version;
    unsigned int rdramSize;
    unsigned int blockCount;
    unsigned int blockSizes[GDX_SS_BLOCK_COUNT];
} GdxSaveStateHeader;

// Single in-RAM slot: [header][RDRAM][block0..blockN]. Lazily (re)allocated on first save.
static unsigned char* sSlot = NULL;
static size_t sSlotSize = 0;
static int sHaveSnapshot = 0;
static int sSaveArmed = 0;
static int sLoadArmed = 0;

static int gdx_savestate_enabled(void) {
    return CVarGetInteger(GDX_SAVESTATE_CVAR, 0) != 0;
}

// Fill `hdr` with the current layout and return the total slot size (header + RDRAM + all blocks).
static size_t gdx_ss_layout(GdxSaveStateHeader* hdr) {
    size_t total;
    unsigned int i;

    hdr->magic = GDX_SS_MAGIC;
    hdr->version = GDX_SS_VERSION;
    hdr->rdramSize = (unsigned int)GDX_RDRAM_SIZE;
    hdr->blockCount = GDX_SS_BLOCK_COUNT;

    total = sizeof(GdxSaveStateHeader) + (size_t)GDX_RDRAM_SIZE;
    for (i = 0; i < GDX_SS_BLOCK_COUNT; i++) {
        unsigned int sz = sBlocks[i].size();
        hdr->blockSizes[i] = sz;
        total += (size_t)sz;
    }
    return total;
}

void gdx_savestate_save(void) {
    // Request only; the copy happens at the parked boundary in gdx_savestate_tick().
    if (!gdx_savestate_enabled()) {
        return; // strict no-op while disabled
    }
    sSaveArmed = 1;
}

void gdx_savestate_load(void) {
    if (!gdx_savestate_enabled()) {
        return; // strict no-op while disabled
    }
    if (!sHaveSnapshot) {
        return; // nothing to restore yet
    }
    sLoadArmed = 1;
}

int gdx_savestate_exists(void) {
    return sHaveSnapshot;
}

static void gdx_ss_do_save(void) {
    GdxSaveStateHeader hdr;
    size_t total;
    size_t off;
    unsigned int i;

    total = gdx_ss_layout(&hdr);

    if (sSlot == NULL || sSlotSize != total) {
        unsigned char* p = (unsigned char*)realloc(sSlot, total);
        if (p == NULL) {
            gdx_port_logf("[savestate] quick-save FAILED: could not allocate %lu-byte slot.\n",
                          (unsigned long)total);
            return;
        }
        sSlot = p;
        sSlotSize = total;
    }

    memcpy(sSlot, &hdr, sizeof(hdr));
    off = sizeof(hdr);

    gdx_audio_ctx_lock();
    memcpy(sSlot + off, gdx_rdram, (size_t)GDX_RDRAM_SIZE);
    off += (size_t)GDX_RDRAM_SIZE;
    for (i = 0; i < GDX_SS_BLOCK_COUNT; i++) {
        sBlocks[i].capture(sSlot + off);
        off += (size_t)hdr.blockSizes[i];
    }
    gdx_audio_ctx_unlock();

    sHaveSnapshot = 1;
    gdx_port_logf("[savestate] quick-save: RDRAM (%lu bytes) + curated race state captured.\n",
                  (unsigned long)GDX_RDRAM_SIZE);
}

static void gdx_ss_do_load(void) {
    GdxSaveStateHeader cur;
    GdxSaveStateHeader stored;
    size_t off;
    unsigned int i;

    if (!sHaveSnapshot || sSlot == NULL) {
        return;
    }

    // Recompute the current layout and validate the stored header against it. In-session these
    // always match (compile-time-constant sizes); the check is a defensive guard that also
    // refuses a stale v1 (RDRAM-only) slot if one were ever present.
    (void)gdx_ss_layout(&cur);
    memcpy(&stored, sSlot, sizeof(stored));
    if (stored.magic != GDX_SS_MAGIC || stored.version != GDX_SS_VERSION ||
        stored.rdramSize != cur.rdramSize || stored.blockCount != cur.blockCount) {
        gdx_port_logf("[savestate] quick-load REFUSED: snapshot layout mismatch.\n");
        return;
    }
    for (i = 0; i < GDX_SS_BLOCK_COUNT; i++) {
        if (stored.blockSizes[i] != cur.blockSizes[i]) {
            gdx_port_logf("[savestate] quick-load REFUSED: block '%s' size mismatch.\n",
                          sBlocks[i].name);
            return;
        }
    }

    off = sizeof(GdxSaveStateHeader);
    gdx_audio_ctx_lock();
    memcpy(gdx_rdram, sSlot + off, (size_t)GDX_RDRAM_SIZE);
    off += (size_t)GDX_RDRAM_SIZE;
    for (i = 0; i < GDX_SS_BLOCK_COUNT; i++) {
        sBlocks[i].restore(sSlot + off);
        off += (size_t)cur.blockSizes[i];
    }
    gdx_audio_ctx_unlock();

    gdx_port_logf("[savestate] quick-load: RDRAM + curated race state restored "
                  "(same-course rewind; audio not rewound).\n");
}

void gdx_savestate_tick(void) {
    // Strict no-op while disabled: no allocation, no copy, no accessor call. Clear any stale arm
    // flags so a request made while enabled, then disabled before this boundary, cannot fire later.
    if (!gdx_savestate_enabled()) {
        sSaveArmed = 0;
        sLoadArmed = 0;
        return;
    }

    if (!sSaveArmed && !sLoadArmed) {
        return;
    }

    // gdx_rdram is initialized before the frame loop; guard anyway so an early call is inert.
    if (gdx_rdram == NULL) {
        sSaveArmed = 0;
        sLoadArmed = 0;
        return;
    }

    // Load takes precedence if both were armed in the same window.
    if (sLoadArmed) {
        sLoadArmed = 0;
        sSaveArmed = 0;
        gdx_ss_do_load();
        return;
    }

    sSaveArmed = 0;
    gdx_ss_do_save();
}

void gdx_savestate_shutdown(void) {
    if (sSlot != NULL) {
        free(sSlot);
        sSlot = NULL;
    }
    sSlotSize = 0;
    sHaveSnapshot = 0;
    sSaveArmed = 0;
    sLoadArmed = 0;
}
