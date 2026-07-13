// port/gdx_ghost_window.cpp — implementation of the "Ghost Browser" GuiWindow.
//
// GROUND-TRUTH INVESTIGATION (why this window is scoped the way it is)
// ------------------------------------------------------------------------------------
// docs/COMING_SOON_ROADMAP.md's Tier 2 Practice item reads: "gGhosts[3]/gGhostRacers[3]
// exist + are pointer-free (racer.c:55,69); build a 'Ghosts' GuiWindow to pick/enable up
// to 3 per course." Two claims need checking against the actual decomp before building
// on top of them, because a wrong assumption here would ship a UI that lies:
//
// 1. "How many ghosts can be SAVED?"
//    decomp/include/fzx_save.h:108-115:
//        typedef struct GhostSave { GhostRecord record; GhostData data; } GhostSave;
//        typedef struct SaveContext { ...; GhostSave ghostSave; ...; } SaveContext;
//    `ghostSave` is a SINGLE GhostSave field, not an array -- gSaveContext holds exactly
//    ONE ghost, for whichever course it was last saved on (Save_SaveGhost overwrites
//    unconditionally; there is no per-course SRAM slot). port/gdx_ghost_io.c's own header
//    comment already documents this ("SCOPE (v1): the single base-course SRAM ghost
//    only"); this window reads that exact same single slot, read-only.
//
//    (COURSE_CONTEXT()->ghostSave[3], decomp/src/overlays/ovl_i2/dd_save.c, IS a 3-slot
//    array, but it belongs to the 64DD/Expansion-Kit MFS working-dir ghost cache, written
//    via func_807684AC/Mfs_* (decomp/src/sys/disk/75000.c). This port's disk emulation
//    (port/disk_buffer.cpp, port/n64_leo.c) only keeps that MFS image IN MEMORY -- there
//    is no host-side write-back (see docs/COMING_SOON_ROADMAP.md's Workshop-tab entry:
//    "MFS saves only hit the in-memory buffer -> evaporate on quit"). So even on a build
//    with a 64DD disk image loaded, anything written into that 3-slot cache does not
//    survive a restart, unlike the SRAM slot (port/sram_buffer.cpp writes fzerox.sav
//    through on every save). This window intentionally does not surface that cache --
//    same out-of-scope call gdx_ghost_io.h already makes, for the same reason.)
//
// 2. "What are gGhosts[3]/gGhostRacers[3], and when are they populated?"
//    decomp/src/game/racer.c:55 `Ghost gGhosts[3];` and :69 `GhostRacer gGhostRacers[3];`
//    are the in-RACE ghost-opponent slots (up to 3 ghosts can run alongside the player in
//    a race). They are filled in two steps:
//      a) decomp/src/overlays/ovl_i2/race.c:63-84 (Race_Init, EXPANSION_KIT build --
//         this port always builds EXPANSION_KIT=1, see port/CMakeLists.txt's
//         GDX_EXPANSION_KIT option): Save_LoadGhost(gCourseIndex) is called ONLY inside
//         `if (gGameMode == GAMEMODE_TIME_ATTACK)` guards (both call sites). Save_LoadGhost
//         (decomp/src/overlays/ovl_i2/save.c:93-122) is what actually copies SRAM/DD ghost
//         bytes into gGhosts[].
//      b) decomp/src/game/racer.c:1871-1929 (also inside Race_Init's call chain): the loop
//         that turns a populated gGhosts[i] into a live gGhostRacers[i] (assigns
//         replayPtr/frameCount/racer slot/etc.) is itself gated on
//         `if (gGameMode == GAMEMODE_TIME_ATTACK)` (racer.c:1878).
//    GAMEMODE_PRACTICE hits neither gate. Every race started in Practice mode runs with
//    gGhostRacers[*].exists == false for all three slots (they are explicitly cleared at
//    racer.c:1871-1873 regardless of mode) -- Practice mode has ZERO ghost opponents
//    today, confirming the prior finding this task was scoped around. There is no
//    "hidden" plumbing already reading gGhosts in Practice; it is a hard no.
//
// WHAT THIS MEANS FOR v1
// ------------------------------------------------------------------------------------
// "Pick up to 3 ghosts to race against in Practice" needs the population path above
// (race.c's two Save_LoadGhost call sites and racer.c:1878's population loop) extended to
// also run for GAMEMODE_PRACTICE, plus a way to stage MORE than the one SRAM ghost into
// gGhosts[0..2] (today Save_LoadGhost only ever has one real candidate: the single SRAM
// slot, or a ROM/DD staff ghost). Both are decomp-side changes outside this window's
// scope (and outside this task's file allowlist -- port/gdx_ghost_window.{h,cpp} only).
// So v1 ships a read-only viewer over the one real, persistent ghost slot plus the
// existing .gdg export shortcut, and says outright -- inside the window, not just in a
// doc -- that Practice-mode ghost opponents are not implemented yet. No dead toggle.

#include "gdx_ghost_window.h"

#include <imgui.h> // vendored in libultraship's imgui; same include used by gdx_menu.cpp / GuiWindow.h
#include <cstdint>
#include <cstdio>  // snprintf (status line, time formatting)
#include <cstring> // memset

#include "gdx_ghost_io.h" // gdx_ghost_export / gdx_ghost_default_path / GDX_GHOST_* (Export button only
                          // -- this window does not touch gdx_ghost_import or the .gdg container format)

// ─────────────────────────────────────────────────────────────────────────────────────────────
// PORT/DECOMP BOUNDARY: byte-for-byte mirror structs + raw `extern` prototypes, same idiom
// port/gdx_ghost_io.c documents at length in its own header comment (read that file first if this
// looks unfamiliar). This window intentionally does NOT #include decomp/include/fzx_save.h --
// doing so would drag in the PORT/EXPANSION_KIT/NON_MATCHING macro-gated declarations the
// gdiffuser_game object library is compiled with, which this G-Diffuser-target file is not. A
// second, independent mirror is kept here (rather than sharing gdx_ghost_io.c's) so this window
// stays a fully self-contained pair of new files, calling only Save_Read* (never Save_Write*/
// Save_LoadGhostInfo's self-healing path -- see "why not Save_LoadGhostInfo" below) and the
// existing gdx_ghost_export() entry point. The size checks below turn any future drift against
// the real decomp structs into a compile error here too.
// ─────────────────────────────────────────────────────────────────────────────────────────────

extern "C" {

// Mirrors MachineInfo, decomp/include/unk_structs.h:43-64. 20 all-u8 fields -> exactly 0x14 bytes.
typedef struct GdxGWMachineInfo {
    uint8_t character;
    uint8_t customType;
    uint8_t frontType;
    uint8_t rearType;
    uint8_t wingType;
    uint8_t logo;
    uint8_t number;
    uint8_t decal;
    uint8_t bodyR;
    uint8_t bodyG;
    uint8_t bodyB;
    uint8_t numberR;
    uint8_t numberG;
    uint8_t numberB;
    uint8_t decalR;
    uint8_t decalG;
    uint8_t decalB;
    uint8_t cockpitR;
    uint8_t cockpitG;
    uint8_t cockpitB;
} GdxGWMachineInfo;

// Mirrors unk_80141C88_unk_1D, decomp/include/unk_structs.h:445-448 (MachineInfo + 12 reserved
// bytes). Exactly 0x20 bytes -- all-u8 members, no padding.
typedef struct GdxGWMachineInfoPadded {
    GdxGWMachineInfo info;
    int8_t reserved[12];
} GdxGWMachineInfoPadded;

// Mirrors GhostRecord, decomp/include/fzx_save.h:74-84. Exactly 0x40 (64) bytes.
typedef struct GdxGWGhostRecord {
    uint16_t checksum;
    uint16_t ghostType;
    int32_t replayChecksum;
    int32_t encodedCourseIndex;
    int32_t raceTime;
    uint16_t unk10;
    int8_t unk12[5];
    uint8_t trackName[9]; // always zeroed by Save_SaveGhostRecord (save.c:1317-1319) -- not
                          // displayed here, it never carries real data on this port.
    GdxGWMachineInfoPadded machine;
} GdxGWGhostRecord;

// Mirrors GhostReplayInfo, decomp/include/fzx_save.h:86-94. Exactly 0x20 (32) bytes.
typedef struct GdxGWGhostReplayInfo {
    uint16_t checksum;
    int16_t unk02;
    int32_t lapTimes[3];
    int32_t end;
    uint32_t size;
    int32_t unk18;
    int32_t unk1C;
} GdxGWGhostReplayInfo;

#define GDX_GW_REPLAY_DATA_SIZE 16200

// Mirrors GhostData, decomp/include/fzx_save.h:96-100. Exactly 0x3F80 (16256) bytes. The window
// only reads replayInfo (checksum + lapTimes) but the full struct is needed so Save_ReadGhostData
// writes the correct number of bytes into it.
typedef struct GdxGWGhostData {
    GdxGWGhostReplayInfo replayInfo;
    uint8_t replayData[GDX_GW_REPLAY_DATA_SIZE];
    int8_t unk3F68[0x18];
} GdxGWGhostData;

typedef char gdx_gw_record_size_check[(sizeof(GdxGWGhostRecord) == 0x40) ? 1 : -1];
typedef char gdx_gw_data_size_check[(sizeof(GdxGWGhostData) == 0x3F80) ? 1 : -1];

// Raw extern declarations, no header include (see boundary note above). These are the same three
// read-only entry points port/gdx_ghost_io.c calls for its own record/data validation step
// (decomp/src/overlays/ovl_i2/save.c:60-68, :1935-1941 by line count in that file's own comment).
//
// Deliberately NOT declared/called here: Save_LoadGhostInfo. It self-heals a bad-checksum slot by
// WRITING a freshly-initialized empty record/data back to SRAM (save.c:746-775) as a side effect
// of "loading" it. That is a reasonable one-shot check for gdx_ghost_io.c's Export button (one
// click = one call), but this window's DrawElement() runs every frame while visible -- calling a
// self-healing write path 60 times a second is not something a read-only browser should do.
// Calling Save_ReadGhostRecord + Save_CalculateGhostRecordChecksum directly (below) gets the same
// "is the slot occupied and valid" answer with a guaranteed-read-only, every-frame-safe path.
extern void Save_ReadGhostRecord(GdxGWGhostRecord* ghostRecord);
extern void Save_ReadGhostData(GdxGWGhostData* ghostData);
extern unsigned short Save_CalculateGhostRecordChecksum(GdxGWGhostRecord* ghostRecord);
extern unsigned short Save_CalculateGhostDataChecksum(GdxGWGhostData* ghostData);

} // extern "C"

// GhostType enum values, decomp/include/fzx_save.h:7-13.
#define GDX_GW_GHOST_NONE 0
#define GDX_GW_GHOST_PLAYER 1
#define GDX_GW_GHOST_STAFF 2
#define GDX_GW_GHOST_CELEBRITY 3
#define GDX_GW_GHOST_CHAMP 4

namespace {

// One frame's worth of what the window needs to draw; recomputed fresh every DrawElement() call
// (cheap -- gdx_sram_read is a plain in-memory memcpy, port/sram_buffer.cpp:109-120, no disk I/O
// on read) so the window always reflects the slot's current contents (e.g. right after using the
// game's own "Save Ghost" prompt or the G-Diffuser autosave-on-record feature).
struct GdxGhostSummary {
    bool occupied = false; // false: slot empty OR checksum invalid (never trust either case)
    int32_t courseIndex = 0;
    int32_t encodedCourseIndex = 0;
    int32_t ghostType = GDX_GW_GHOST_NONE;
    int32_t raceTime = 0;
    GdxGWMachineInfo machine = {};

    bool haveLapTimes = false; // GhostData checksum valid
    int32_t bestLapTime = 0;
};

// Mirrors func_i2_80101590's derivation, decomp/src/overlays/ovl_i2/save.c:777-792:
// courseIndex = encodedCourseIndex & 0x1F.
int32_t GdxDeriveCourseIndex(int32_t encodedCourseIndex) {
    return encodedCourseIndex & 0x1F;
}

// Reads the current SRAM ghost slot, read-only (see the "why not Save_LoadGhostInfo" note above).
// A slot is considered occupied only if its checksum is self-consistent AND encodedCourseIndex is
// non-zero -- the same two conditions Save_LoadGhostInfo itself treats as "a real ghost is here"
// (save.c:760-774: a bad checksum or encodedCourseIndex == 0 both mean "nothing usable").
GdxGhostSummary GdxReadGhostSummary() {
    GdxGhostSummary out;

    GdxGWGhostRecord record;
    memset(&record, 0, sizeof(record));
    Save_ReadGhostRecord(&record);

    unsigned short recordCk = Save_CalculateGhostRecordChecksum(&record);
    if (recordCk != record.checksum || record.encodedCourseIndex == 0) {
        return out; // occupied stays false
    }

    out.occupied = true;
    out.encodedCourseIndex = record.encodedCourseIndex;
    out.courseIndex = GdxDeriveCourseIndex(record.encodedCourseIndex);
    out.ghostType = record.ghostType;
    out.raceTime = record.raceTime;
    out.machine = record.machine.info;

    GdxGWGhostData data;
    memset(&data, 0, sizeof(data));
    Save_ReadGhostData(&data);
    unsigned short dataCk = Save_CalculateGhostDataChecksum(&data);
    if (dataCk == data.replayInfo.checksum) {
        int32_t best = -1;
        for (int i = 0; i < 3; i++) {
            int32_t lap = data.replayInfo.lapTimes[i];
            if (lap > 0 && (best < 0 || lap < best)) {
                best = lap;
            }
        }
        if (best >= 0) {
            out.haveLapTimes = true;
            out.bestLapTime = best;
        }
    }

    return out;
}

// raceTime/lapTimes are milliseconds (decomp/src/overlays/ovl_i3/hud.c:320,335,353 divides by
// 60000/1000/100 to render minutes/seconds/hundredths). Formats as M'SS.mmm.
const char* GdxFormatTime(int32_t timeMs, char* buf, size_t bufSize) {
    if (timeMs < 0) {
        timeMs = 0;
    }
    int32_t minutes = timeMs / 60000;
    int32_t seconds = (timeMs / 1000) % 60;
    int32_t millis = timeMs % 1000;
    snprintf(buf, bufSize, "%d'%02d.%03d", minutes, seconds, millis);
    return buf;
}

const char* GdxGhostTypeName(int32_t ghostType) {
    switch (ghostType) {
        case GDX_GW_GHOST_PLAYER:
            return "Player";
        case GDX_GW_GHOST_STAFF:
            return "Staff";
        case GDX_GW_GHOST_CELEBRITY:
            return "Celebrity";
        case GDX_GW_GHOST_CHAMP:
            return "Champ";
        default:
            return "Unknown";
    }
}

} // namespace

void GdxGhostWindow::InitElement() {
}

void GdxGhostWindow::UpdateElement() {
}

void GdxGhostWindow::DrawElement() {
    ImGui::TextWrapped("Browses the port's one persisted ghost save slot (gSaveContext.ghostSave). "
                        "This is a read-only viewer -- it does not race against you yet; see the "
                        "note at the bottom.");
    ImGui::Separator();

    GdxGhostSummary summary = GdxReadGhostSummary();

    if (!summary.occupied) {
        ImGui::TextDisabled("No ghost is currently saved.");
        ImGui::TextWrapped("Save one from a race (the game's own \"Save Ghost\" prompt, or the "
                            "G-Diffuser autosave-on-record option in the Gameplay tab), then reopen "
                            "this window.");
    } else {
        char timeBuf[32];
        char lapBuf[32];

        if (ImGui::BeginTable("GdxGhostSummaryTable", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Course index");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d  (encoded 0x%08X)", summary.courseIndex, (unsigned int) summary.encodedCourseIndex);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Ghost type");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", GdxGhostTypeName(summary.ghostType));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Total time");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", GdxFormatTime(summary.raceTime, timeBuf, sizeof(timeBuf)));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Best lap");
            ImGui::TableSetColumnIndex(1);
            if (summary.haveLapTimes) {
                ImGui::Text("%s", GdxFormatTime(summary.bestLapTime, lapBuf, sizeof(lapBuf)));
            } else {
                ImGui::TextDisabled("unavailable (replay data checksum invalid)");
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Machine");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("Character #%d", (int) summary.machine.character);
            ImGui::SameLine();
            ImVec4 bodyColor(summary.machine.bodyR / 255.0f, summary.machine.bodyG / 255.0f,
                              summary.machine.bodyB / 255.0f, 1.0f);
            ImGui::ColorButton("##GdxGhostBodyColor", bodyColor,
                                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(14, 14));
            ImGui::SameLine();
            ImGui::TextDisabled("body color");

            ImGui::EndTable();
        }

        ImGui::Separator();

        // Reuses the existing gdx_ghost_io.h public API verbatim -- no export/checksum/container
        // logic duplicated here. Mirrors gdx_menu.cpp's own Export button (Practice tab).
        static char sExportStatus[192] = { 0 };
        if (ImGui::Button("Export to .gdg")) {
            char path[1024];
            if (!gdx_ghost_default_path(path, sizeof(path))) {
                snprintf(sExportStatus, sizeof(sExportStatus), "Export failed: could not resolve output path.");
            } else {
                int rc = gdx_ghost_export(GDX_GHOST_ANY_COURSE, path);
                if (rc == GDX_GHOST_OK) {
                    snprintf(sExportStatus, sizeof(sExportStatus), "Exported to %s", path);
                } else if (rc == GDX_GHOST_ERR_NO_GHOST) {
                    snprintf(sExportStatus, sizeof(sExportStatus), "Export: no ghost is saved yet.");
                } else {
                    snprintf(sExportStatus, sizeof(sExportStatus), "Export failed (code %d).", rc);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Writes the currently-saved ghost replay to a .gdg file next to the exe.\n"
                              "Same file gdx_ghost_io.c reads back on Import (Practice tab).");
        }
        if (sExportStatus[0] != '\0') {
            ImGui::TextWrapped("%s", sExportStatus);
        }
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Practice-mode ghost opponents are not implemented yet.");
    ImGui::TextWrapped(
        "F-Zero X only loads ghosts into gGhosts[3]/gGhostRacers[3] during Time Attack -- "
        "Race_Init (decomp/src/overlays/ovl_i2/race.c) calls Save_LoadGhost only when "
        "gGameMode == GAMEMODE_TIME_ATTACK, and the race-init ghost-racer population loop "
        "(decomp/src/game/racer.c:1878) has the same gate. Practice mode never populates "
        "either array, so there is currently no ghost to \"enable\" here. Picking up to 3 "
        "ghosts to race against in Practice needs that population path extended to Practice "
        "mode first -- tracked as follow-up work, not shipped here as a non-functional toggle.");
}
