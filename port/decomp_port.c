// G-Diffuser — decomp-side port subsystems (Slice 4c).
// Compiled WITH the decomp's headers/flags (part of the gdiffuser_game target), so it uses
// the real game types. Provides port reimplementations / placeholders for symbols that lived
// in N64-platform files excluded from the host build (segment system, save, fixed-address
// data). Placeholders let the exe LINK and boot; real backing (resource system, LUS save)
// comes next.

#include "global.h"

// ---- Segment system (port reimplementation) --------------------------------
// N64 mapped 16 graphics segments to RDRAM; the game addresses assets as (segment<<24|offset).
// On host we store REAL pointers per segment (populated as assets load) and translate directly
// — no N64 KSEG0 (PHYS_TO_K0) mapping.
uintptr_t gSegments[16];

uintptr_t Segment_SegmentedToVirtual(uintptr_t segmentedAddr) {
    return gSegments[(segmentedAddr >> 24) & 0xF] + (segmentedAddr & 0x00FFFFFF);
}

// ---- Save system (stubbed for first boot; back with libultraship save later) ----
s32 Save_LoadStaffGhostRecord(GhostInfo* ghostInfo, s32 courseIndex) {
    (void)ghostInfo;
    (void)courseIndex;
    return -1; // no record
}

s32 Save_SaveSettingsProfiles(void) {
    return 0; // pretend success
}

// ---- Graphics pool ---------------------------------------------------------
// D_1000000: the N64 graphics pool (segment 0x01) — a real runtime buffer (NOT an o2r asset),
// so display-list/matrix allocations have somewhere to live.
// (aVp* viewports and D_80149A0 are real assets now provided by the R2 asset bindings.)
GfxPool D_1000000;
