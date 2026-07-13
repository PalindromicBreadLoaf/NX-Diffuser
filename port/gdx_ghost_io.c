/* port/gdx_ghost_io.c -- .gdg ghost replay import/export implementation.
 *
 * See gdx_ghost_io.h for the file format, endianness policy, validation rules, and
 * overwrite policy. This file lives in the G-Diffuser host-CRT target (add_executable
 * (G-Diffuser ...) in port/CMakeLists.txt), the same target as port/sram_buffer.cpp and
 * port/gdx_menu.cpp -- NOT the gdiffuser_game decomp object library -- so it can freely
 * use the standard C file API (fopen/fread/fwrite), matching sram_buffer.cpp's own
 * "host .cpp/.c TU, not decomp-target" split.
 *
 * PORT/DECOMP BOUNDARY: this file needs the exact GhostRecord/GhostData/GhostInfo
 * layouts (decomp/include/fzx_save.h, decomp/include/unk_structs.h) and calls straight
 * into decomp/src/overlays/ovl_i2/save.c's real Save_Read*, Save_Write*, and Save_Calculate*
 * functions -- but it deliberately does NOT #include the decomp headers. Pulling in
 * fzx_save.h here would also pull in unk_structs.h and the PORT/EXPANSION_KIT/
 * NON_MATCHING/VERSION_US macro-gated declarations the gdiffuser_game object library is
 * compiled with (see port/CMakeLists.txt's target_compile_definitions(gdiffuser_game...)
 * ), none of which are (or should be) defined for this G-Diffuser-target file. Instead,
 * this file declares its OWN byte-for-byte mirror structs below and raw `extern`
 * prototypes for the handful of save.c entry points it calls -- the exact same
 * boundary-crossing idiom port/sram_buffer.cpp already uses for Sram_ReadWrite ("Raw
 * extern declarations only (no header include) -- decomp-target C files can't include
 * the MSVC CRT headers port/sram_buffer.cpp uses"). A C function call only needs the
 * pointee's SIZE and FIELD OFFSETS to agree across the two translation units (the
 * pointer itself is just an address); it does not need the struct tag name to match.
 * The gdx_ghost_*_size_check typedefs below turn any future drift between this mirror
 * and the real structs into a compile error here, not silent SRAM corruption.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fread/fwrite below; harmless on non-MSVC */

#include "gdx_ghost_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ---------------------------------------------------------------------------------
 * Mirrored decomp ghost structs.
 *
 * Field names/comments cite the real declarations so this stays easy to audit against
 * decomp/include/fzx_save.h and decomp/include/unk_structs.h if either ever changes.
 * All fields are u8/s8/u16/s16/u32/s32-equivalent (fixed-width <stdint.h> types here,
 * matching decomp's PR/ultratypes.h typedefs on this port's only current target: MSVC,
 * Windows, LLP64, where u32/s32 == unsigned long/long == 4 bytes, same as uint32_t/
 * int32_t) with natural alignment only -- no #pragma pack is needed because every
 * struct below already lands on the same offsets/total sizes as the "// size = 0x.."
 * comments in the real headers (verified by hand; see the static size checks below).
 * ------------------------------------------------------------------------------- */

/* Mirrors MachineInfo, decomp/include/unk_structs.h:43-64. 20 fields, all u8 -> no
 * alignment padding possible; size is exactly 0x14 (20) bytes. */
typedef struct GdxMachineInfo {
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
} GdxMachineInfo;

/* Mirrors unk_80141C88_unk_1D, decomp/include/unk_structs.h:445-448 (MachineInfo +
 * 12 reserved bytes). Size is exactly 0x20 (32) bytes -- all-u8 members, no padding. */
typedef struct GdxMachineInfoPadded {
    GdxMachineInfo info;
    int8_t reserved[12];
} GdxMachineInfoPadded;

/* Mirrors GhostRecord, decomp/include/fzx_save.h:74-84. Size 0x40 (64) bytes: the s32
 * fields at offsets 4/8/12 are already 4-byte aligned with no gaps, and the struct's
 * total size (64) is already a multiple of 4, so no trailing padding is inserted. */
typedef struct GdxGhostRecord {
    uint16_t checksum;
    uint16_t ghostType;
    int32_t replayChecksum;
    int32_t encodedCourseIndex;
    int32_t raceTime;
    uint16_t unk10;
    int8_t unk12[5];
    uint8_t trackName[9];
    GdxMachineInfoPadded machine; /* GhostRecord.unk_20 */
} GdxGhostRecord;

/* Mirrors GhostReplayInfo, decomp/include/fzx_save.h:86-94. Size 0x20 (32) bytes. */
typedef struct GdxGhostReplayInfo {
    uint16_t checksum;
    int16_t unk02;
    int32_t lapTimes[3];
    int32_t end;
    uint32_t size;
    int32_t unk18;
    int32_t unk1C;
} GdxGhostReplayInfo;

#define GDX_GHOST_REPLAY_DATA_SIZE 16200

/* Mirrors GhostData, decomp/include/fzx_save.h:96-100. Size 0x3F80 (16256) bytes. */
typedef struct GdxGhostData {
    GdxGhostReplayInfo replayInfo;
    uint8_t replayData[GDX_GHOST_REPLAY_DATA_SIZE];
    int8_t unk3F68[0x18];
} GdxGhostData;

/* Mirrors GhostSave, decomp/include/fzx_save.h:108-111. Size 0x3FC0 (16320) bytes --
 * this is exactly the ".gdg payload" described in gdx_ghost_io.h. */
typedef struct GdxGhostSave {
    GdxGhostRecord record;
    GdxGhostData data;
} GdxGhostSave;

/* Mirrors GhostInfo, decomp/include/unk_structs.h:450-459. Real size is 0x40 (64)
 * bytes: the field layout only sums to 0x3D (61) bytes, but GhostInfo contains s32
 * members, so the compiler pads the overall struct size up to the next multiple of 4
 * (61 -> 64). This mirror only needs the leading `courseIndex` field for the overwrite-
 * safety check below, but the full field list is kept for clarity/future use. */
typedef struct GdxGhostInfo {
    int32_t courseIndex;
    int32_t encodedCourseIndex;
    int32_t raceTime;
    int32_t replayChecksum;
    uint16_t ghostType;
    uint16_t unk12;
    char trackName[9];
    GdxMachineInfoPadded unk1D;
} GdxGhostInfo;

/* Defensive compile-time size checks: a negative array size is a compile error, so any
 * future drift between these mirrors and the real decomp structs fails loudly here
 * instead of silently corrupting SRAM through a mis-sized extern call. */
typedef char gdx_ghost_record_size_check[(sizeof(GdxGhostRecord) == 0x40) ? 1 : -1];
typedef char gdx_ghost_data_size_check[(sizeof(GdxGhostData) == 0x3F80) ? 1 : -1];
typedef char gdx_ghost_save_size_check[(sizeof(GdxGhostSave) == 0x3FC0) ? 1 : -1];

/* GhostType enum values, decomp/include/fzx_save.h:7-13 (GHOST_NONE excluded here --
 * it marks an empty slot, not a real ghost, so importing one is rejected). */
#define GDX_GHOST_TYPE_PLAYER 1
#define GDX_GHOST_TYPE_STAFF 2
#define GDX_GHOST_TYPE_CELEBRITY 3
#define GDX_GHOST_TYPE_CHAMP 4

/* COURSE_EDIT_1, decomp/include/fzx_course.h:74 -- the base-course range is [0, 24);
 * this is the same bound Gdx_AutosaveGhostOnRecord and Menus_AttemptSaveGhost use to
 * gate Save_SaveGhost (decomp/src/overlays/ovl_i3/menus.c). */
#define GDX_GHOST_MAX_COURSE_INDEX 24

/* ---------------------------------------------------------------------------------
 * Real decomp entry points (decomp/src/overlays/ovl_i2/save.c). Raw extern
 * declarations, no header include -- see the file header comment above for why. All
 * seven are plain (non-static) C functions, unconditionally compiled (EXPANSION_KIT is
 * always on for this port -- port/CMakeLists.txt's GDX_EXPANSION_KIT option defaults
 * ON), so the EXPANSION_KIT branch inside each of them is always the one in effect:
 * they operate on the single live gSaveContext.ghostSave (record/data) whenever their
 * argument happens to point at it (Save_LoadGhostInfo, via Save_ReadGhostRecord
 * internally), or on whatever buffer the caller passes (Save_Read/WriteGhostRecord/
 * Data, Save_CalculateGhost*Checksum -- these four never touch gSaveContext directly,
 * only through the pointer given to them, which is why this file can round-trip a
 * local GdxGhostSave buffer without ever needing to see gSaveContext itself). s32/u16
 * return types are declared here as int/unsigned short -- on this port's only current
 * target (MSVC/LLP64) that is byte-identical to decomp's s32 (long, 4 bytes)/u16
 * (unsigned short, 2 bytes).
 */
extern int Save_LoadGhostInfo(GdxGhostInfo* ghostInfo);
extern void Save_ReadGhostRecord(GdxGhostRecord* ghostRecord);
extern void Save_ReadGhostData(GdxGhostData* ghostData);
extern void Save_WriteGhostRecord(GdxGhostRecord* ghostRecord);
extern void Save_WriteGhostData(GdxGhostData* ghostData);
extern unsigned short Save_CalculateGhostRecordChecksum(GdxGhostRecord* ghostRecord);
extern unsigned short Save_CalculateGhostDataChecksum(GdxGhostData* ghostData);

/* ---------------------------------------------------------------------------------
 * .gdg container header: fixed 20-byte, explicit little-endian, hand-packed (see the
 * format comment in gdx_ghost_io.h). Independent of host struct layout/alignment.
 * ------------------------------------------------------------------------------- */

#define GDX_GHOST_HEADER_SIZE 20
#define GDX_GHOST_FORMAT_VERSION 1u

static void gdx_write_u32le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static uint32_t gdx_read_u32le(const unsigned char* p) {
    uint32_t v;
    v = (uint32_t) p[0];
    v |= (uint32_t) p[1] << 8;
    v |= (uint32_t) p[2] << 16;
    v |= (uint32_t) p[3] << 24;
    return v;
}

static void gdx_ghost_pack_header(unsigned char* header, int32_t courseId, uint32_t payloadSize, uint32_t crc) {
    header[0] = 'G';
    header[1] = 'D';
    header[2] = 'G';
    header[3] = '1';
    gdx_write_u32le(header + 4, GDX_GHOST_FORMAT_VERSION);
    gdx_write_u32le(header + 8, (uint32_t) courseId);
    gdx_write_u32le(header + 12, payloadSize);
    gdx_write_u32le(header + 16, crc);
}

/* ---------------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320), reflected, table-based. Standard,
 * public-domain algorithm, computed lazily into a static table on first use.
 * ------------------------------------------------------------------------------- */

static uint32_t gdx_crc32(const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static int tableReady = 0;
    uint32_t crc;
    size_t i;

    if (!tableReady) {
        uint32_t c;
        unsigned int n;
        unsigned int k;
        for (n = 0; n < 256; n++) {
            c = (uint32_t) n;
            for (k = 0; k < 8; k++) {
                if (c & 1u) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[n] = c;
        }
        tableReady = 1;
    }

    crc = 0xFFFFFFFFu;
    for (i = 0; i < length; i++) {
        crc = table[(crc ^ (uint32_t) data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------------- */

int gdx_ghost_export(int courseIndex, const char* path) {
    GdxGhostInfo info;
    GdxGhostSave* save;
    FILE* f;
    unsigned char header[GDX_GHOST_HEADER_SIZE];
    uint32_t crc;
    int loadResult;
    unsigned short recordCk;
    unsigned short dataCk;
    int rc;

    if (path == NULL || path[0] == '\0') {
        return GDX_GHOST_ERR_BAD_ARGS;
    }

    memset(&info, 0, sizeof(info));
    loadResult = Save_LoadGhostInfo(&info);
    if (loadResult != 0) {
        /* Non-zero: the slot was empty, or had a bad checksum and Save_LoadGhostInfo
         * just self-healed it back to empty (this mirrors how Gdx_AutosaveGhostOnRecord
         * reads occupancy). Either way there is nothing valid to export. */
        return GDX_GHOST_ERR_NO_GHOST;
    }
    if (courseIndex != GDX_GHOST_ANY_COURSE && courseIndex != info.courseIndex) {
        return GDX_GHOST_ERR_COURSE_MISMATCH;
    }

    save = (GdxGhostSave*) malloc(sizeof(GdxGhostSave));
    if (save == NULL) {
        return GDX_GHOST_ERR_IO;
    }
    memset(save, 0, sizeof(*save));

    Save_ReadGhostRecord(&save->record);
    recordCk = Save_CalculateGhostRecordChecksum(&save->record);
    if (recordCk != save->record.checksum) {
        /* Should not happen right after a successful Save_LoadGhostInfo, but never
         * export a record this port cannot itself validate. */
        free(save);
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }

    Save_ReadGhostData(&save->data);
    dataCk = Save_CalculateGhostDataChecksum(&save->data);
    if (dataCk != save->data.replayInfo.checksum) {
        free(save);
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        free(save);
        return GDX_GHOST_ERR_IO;
    }

    crc = gdx_crc32((const unsigned char*) save, sizeof(*save));
    gdx_ghost_pack_header(header, info.courseIndex, (uint32_t) sizeof(*save), crc);

    rc = GDX_GHOST_OK;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) || fwrite(save, 1, sizeof(*save), f) != sizeof(*save)) {
        rc = GDX_GHOST_ERR_IO;
    }
    fclose(f);
    free(save);

    if (rc != GDX_GHOST_OK) {
        remove(path); /* do not leave a truncated/partial .gdg behind */
    }
    return rc;
}

int gdx_ghost_import(const char* path) {
    FILE* f;
    unsigned char header[GDX_GHOST_HEADER_SIZE];
    GdxGhostSave* save;
    GdxGhostInfo current;
    uint32_t payloadSize;
    uint32_t storedCrc;
    uint32_t computedCrc;
    int32_t headerCourse;
    int32_t derivedCourse;
    unsigned short recordCk;
    unsigned short dataCk;
    int currentResult;

    if (path == NULL || path[0] == '\0') {
        return GDX_GHOST_ERR_BAD_ARGS;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_GHOST_ERR_IO;
    }

    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }

    if (header[0] != 'G' || header[1] != 'D' || header[2] != 'G' || header[3] != '1') {
        fclose(f);
        return GDX_GHOST_ERR_BAD_MAGIC;
    }
    if (gdx_read_u32le(header + 4) != GDX_GHOST_FORMAT_VERSION) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_VERSION;
    }

    headerCourse = (int32_t) gdx_read_u32le(header + 8);
    payloadSize = gdx_read_u32le(header + 12);
    storedCrc = gdx_read_u32le(header + 16);

    if (payloadSize != (uint32_t) sizeof(GdxGhostSave)) {
        fclose(f);
        return GDX_GHOST_ERR_BAD_SIZE;
    }

    save = (GdxGhostSave*) malloc(sizeof(GdxGhostSave));
    if (save == NULL) {
        fclose(f);
        return GDX_GHOST_ERR_IO;
    }

    if (fread(save, 1, sizeof(*save), f) != sizeof(*save)) {
        fclose(f);
        free(save);
        return GDX_GHOST_ERR_IO;
    }
    fclose(f);

    computedCrc = gdx_crc32((const unsigned char*) save, sizeof(*save));
    if (computedCrc != storedCrc) {
        free(save);
        return GDX_GHOST_ERR_BAD_CONTAINER_CRC;
    }

    /* The real import sanity check: the game's own checksum routines, called directly
     * (decomp/src/overlays/ovl_i2/save.c:1935-1941) -- not reimplemented here. */
    recordCk = Save_CalculateGhostRecordChecksum(&save->record);
    if (recordCk != save->record.checksum) {
        free(save);
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }
    dataCk = Save_CalculateGhostDataChecksum(&save->data);
    if (dataCk != save->data.replayInfo.checksum) {
        free(save);
        return GDX_GHOST_ERR_BAD_CHECKSUM;
    }

    if (save->record.ghostType < GDX_GHOST_TYPE_PLAYER || save->record.ghostType > GDX_GHOST_TYPE_CHAMP) {
        free(save);
        return GDX_GHOST_ERR_BAD_GHOST_TYPE;
    }

    /* Mirrors func_i2_80101590 (save.c:777-792): courseIndex = encodedCourseIndex & 0x1F. */
    derivedCourse = save->record.encodedCourseIndex & 0x1F;
    if (save->record.encodedCourseIndex == 0 || derivedCourse < 0 || derivedCourse >= GDX_GHOST_MAX_COURSE_INDEX) {
        free(save);
        return GDX_GHOST_ERR_BAD_COURSE;
    }
    if (headerCourse != derivedCourse) {
        /* Header and payload disagree -- foreign or hand-edited file; refuse it rather
         * than guessing which one is right. */
        free(save);
        return GDX_GHOST_ERR_BAD_COURSE;
    }

    memset(&current, 0, sizeof(current));
    currentResult = Save_LoadGhostInfo(&current);
    if (currentResult == 0 && current.courseIndex != derivedCourse) {
        /* Slot is occupied by a DIFFERENT course's ghost -- never overwritten silently. */
        free(save);
        return GDX_GHOST_ERR_COURSE_MISMATCH;
    }

    /* Install: write through the game's own SRAM helpers -- the same two calls
     * Save_SaveGhost makes, and therefore the same port/sram_buffer.cpp write-through
     * path to fzerox.sav the autosave-on-record feature uses. Writing the parsed
     * record/data directly (instead of routing through a runtime Ghost struct the way
     * Save_SaveGhost(courseIndex, Ghost*) does) keeps trackName/unk_12 byte-identical to
     * what was exported -- see the header comment for why. Both calls recompute the
     * checksum fields themselves; since step 3 above already confirmed those checksums
     * are correct for this exact payload, the recomputation is a no-op check, not a
     * behavior change. */
    Save_WriteGhostRecord(&save->record);
    Save_WriteGhostData(&save->data);

    free(save);
    return GDX_GHOST_OK;
}

int gdx_ghost_default_path(char* outPath, size_t outCap) {
    if (outPath == NULL || outCap == 0) {
        return 0;
    }

#ifdef _WIN32
    {
        char exePath[MAX_PATH];
        char* slash;
        size_t exeDirLen;
        size_t fileNameLen;
        DWORD n;

        n = GetModuleFileNameA(NULL, exePath, (DWORD) sizeof(exePath));
        if (n == 0 || n >= sizeof(exePath)) {
            return 0;
        }
        slash = strrchr(exePath, '\\');
        if (slash == NULL) {
            return 0;
        }
        exeDirLen = (size_t) (slash - exePath) + 1; /* keep the trailing backslash */
        fileNameLen = strlen(GDX_GHOST_DEFAULT_FILENAME);
        if (exeDirLen + fileNameLen + 1 > outCap) {
            return 0;
        }
        memcpy(outPath, exePath, exeDirLen);
        memcpy(outPath + exeDirLen, GDX_GHOST_DEFAULT_FILENAME, fileNameLen + 1); /* + NUL */
        return 1;
    }
#else
    if (strlen(GDX_GHOST_DEFAULT_FILENAME) + 1 > outCap) {
        return 0;
    }
    strcpy(outPath, GDX_GHOST_DEFAULT_FILENAME); /* CWD-relative fallback */
    return 1;
#endif
}
