// port/gdx_savestate.h -- in-session (RAM) quick-save / quick-load (curated same-race rewind).
//
// Snapshots the 16 MB emulated RDRAM buffer PLUS a curated set of the game's
// race-critical native globals (machine physics, racers, cameras, RNG, race
// progress, game-mode flow), so a same-race quick-load actually rewinds the cars.
// Read the header comment in gdx_savestate.c before enabling: it documents the
// pointer-safety analysis, the SAME-COURSE / SAME-RACE constraint, and what does
// NOT rewind (audio phase, some overlay/HUD state). The whole feature is gated
// behind the CVar gEnhancements.Gameplay.SaveStates (default 0 = strict no-op:
// no allocation, no copy, no behavior change).
//
// The public save/load calls are REQUEST functions: they only ARM a flag. The
// actual snapshot/restore runs at the frame-loop boundary in gdx_savestate_tick(),
// the one point where every decomp game fiber is provably parked.

#ifndef GDX_SAVESTATE_H
#define GDX_SAVESTATE_H

#ifdef __cplusplus
extern "C" {
#endif

// Arm a quick-save. No-op unless the SaveStates CVar is enabled. The snapshot
// itself is taken at the next gdx_savestate_tick() boundary.
void gdx_savestate_save(void);

// Arm a quick-load. No-op unless the CVar is enabled AND a snapshot exists. The
// restore is applied at the next gdx_savestate_tick() boundary.
void gdx_savestate_load(void);

// Non-zero once a snapshot has been captured this session (drives the menu's
// Load-button enabled state). 0 before the first successful save.
int gdx_savestate_exists(void);

// Frame-loop boundary hook. MUST be called once per frame right after
// gdx_dispatch() returns (see port/main.cpp), where all decomp game fibers are
// parked at their retrace wait. Strict no-op while the CVar is 0.
void gdx_savestate_tick(void);

// Release the in-RAM slot. Call once at shutdown. Safe when nothing was allocated.
void gdx_savestate_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // GDX_SAVESTATE_H
