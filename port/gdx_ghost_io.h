#ifndef GDIFFUSER_GDX_GHOST_IO_H
#define GDIFFUSER_GDX_GHOST_IO_H

#include <stddef.h>

/* port/gdx_ghost_io.h -- .gdg ghost replay import/export (G-Diffuser Practice tab).
 *
 * Lets a player save the game's single-slot SRAM ghost (the one built by the vanilla
 * "Save Ghost" prompt and by the G-Diffuser autosave-on-record feature, both of which
 * call Save_SaveGhost -- see decomp/src/overlays/ovl_i3/menus.c's Menus_AttemptSaveGhost
 * and Gdx_AutosaveGhostOnRecord) out to a portable file, and load one back in.
 *
 * SCOPE (v1): the single base-course SRAM ghost only (courses 0..23, gSaveContext.
 * ghostSave -- decomp/include/fzx_save.h:74-119). This intentionally does NOT touch the
 * 64DD/Expansion-Kit per-course ghost cache (COURSE_CONTEXT()->ghostSave[i], DDSave_*
 * in decomp/src/overlays/ovl_i2/dd_save.c) -- that path is dead on the port (no real 64DD
 * drive) and is out of scope for this ticket. docs/ONLINE_ECOSYSTEM_BLUEPRINT.md:223-278
 * sketches a fuller ".gdg" container (JSON metadata, optional input-replay chunks, a
 * ghost registry) for a later P1 slice; this header implements just the "GDG1" magic +
 * verbatim-payload core of that design, which is forward-compatible with it (a v1 file
 * is a strict prefix of what a later version could still call "GDG1" data, but this
 * loader intentionally rejects anything that isn't byte-for-byte the v1 shape below --
 * see "Format versioning").
 *
 * .gdg FILE FORMAT (v1)
 * ----------------------
 * All integers in the 20-byte header are little-endian, hand-packed (2/4-byte fields
 * written LSB-first), independent of host struct layout -- this is a NEW container
 * format invented for this feature, so its byte order is defined explicitly rather than
 * left to "whatever the compiler does":
 *
 *   offset  size  field
 *   0       4     magic       ASCII "GDG1" (no NUL terminator)
 *   4       4     version     u32 LE, must be 1
 *   8       4     courseId    s32 LE, redundant copy of (encodedCourseIndex & 0x1F) --
 *                             lets a browser/importer sanity-check a file without fully
 *                             parsing the payload; the importer also cross-checks this
 *                             against the payload's own encodedCourseIndex and rejects
 *                             the file if they disagree.
 *   12      4     payloadSize u32 LE, must equal sizeof(GhostRecord) + sizeof(GhostData)
 *                             = 0x40 + 0x3F80 = 0x3FC0 for v1.
 *   16      4     crc32       u32 LE, IEEE 802.3 CRC-32 of the payload bytes (offset 20
 *                             onward), catching file-transfer corruption/truncation
 *                             independent of the game's own record/data checksums below.
 *   20      0x3FC0 payload    GhostRecord (0x40 bytes) immediately followed by GhostData
 *                             (0x3F80 bytes) -- see decomp/include/fzx_save.h:74-100 --
 *                             copied VERBATIM: the exact bytes Save_ReadGhostRecord /
 *                             Save_ReadGhostData produce, in this port's native in-memory
 *                             struct layout.
 *
 * Total v1 file size: 20 + 0x3FC0 = 0x3FD4 (16340) bytes, always (fixed-size payload;
 * there is no variable-length data in a base-course ghost).
 *
 * ENDIANNESS of the payload: this port persists gSaveContext (and therefore the ghost
 * SRAM slot) to fzerox.sav as a raw, un-byteswapped memcpy of the in-memory struct --
 * see port/sram_buffer.cpp's gdx_sram_read/gdx_sram_write, which never touch the byte
 * order of what they copy. The port currently only targets little-endian hosts (x86/
 * x64; port/sram_buffer.cpp's own comment notes persistence is Windows-only so far), so
 * "the payload's native byte order" and "little-endian" are the same thing today. The
 * .gdg payload therefore intentionally matches fzerox.sav's own convention (this is also
 * what docs/ONLINE_ECOSYSTEM_BLUEPRINT.md:230 specifies: "GhostSave: 0x3FC0 native bytes
 * -- GhostRecord + GhostData, verbatim") rather than re-encoding every u16/s32 field by
 * hand, which would just be a second, independent place that byte order could get out of
 * sync with the SRAM path it mirrors. If this port ever grows a big-endian host target,
 * the payload would need an explicit per-field re-encode at that point -- this is called
 * out here so it is not a silent trap.
 *
 * VALIDATION on import (gdx_ghost_import), in order -- any failure leaves the current
 * SRAM ghost slot and fzerox.sav completely untouched:
 *   1. magic == "GDG1", version == 1, payloadSize == 0x3FC0.
 *   2. crc32 matches the payload bytes actually read (container integrity).
 *   3. GhostRecord.checksum matches Save_CalculateGhostRecordChecksum(record) and
 *      GhostData.replayInfo.checksum matches Save_CalculateGhostDataChecksum(data) --
 *      THE GAME'S OWN checksum routines (decomp/src/overlays/ovl_i2/save.c:1935-1941),
 *      called directly, not reimplemented -- this is the "game's own checksum validation
 *      becomes the import sanity check" (docs/ONLINE_ECOSYSTEM_BLUEPRINT.md:244-245).
 *   4. The derived course (encodedCourseIndex & 0x1F, mirroring save.c's
 *      func_i2_80101590) is in range [0, 24) and matches the header's courseId.
 *   5. ghostType is one of the defined GhostType values (fzx_save.h:7-13), excluding
 *      GHOST_NONE (0), which marks an empty slot, not a real ghost.
 *
 * OVERWRITE POLICY on import: the SRAM ghost slot is single-slot (one ghost total,
 * whichever course it was last saved for -- see Save_LoadGhostInfo / GhostRecord.
 * encodedCourseIndex). Import queries the slot the same way Gdx_AutosaveGhostOnRecord
 * does (Save_LoadGhostInfo): an empty/self-healed slot is always free to write; an
 * occupied slot may only be overwritten if it already holds a ghost for the SAME course
 * as the file being imported (an explicit, user-initiated action). A different course's
 * ghost is NEVER silently replaced -- import fails with GDX_GHOST_ERR_COURSE_MISMATCH
 * and nothing is written.
 *
 * PERSISTENCE: a successful import writes through Save_WriteGhostRecord /
 * Save_WriteGhostData -- the same two calls Save_SaveGhost makes, and therefore the same
 * port/sram_buffer.cpp write-through path to fzerox.sav the autosave-on-record feature
 * uses (Gdx_AutosaveGhostOnRecord, decomp/src/overlays/ovl_i3/menus.c). Import
 * deliberately writes the raw record/data verbatim instead of routing through a runtime
 * Ghost struct the way Save_SaveGhost(courseIndex, Ghost*) does: Save_SaveGhostRecord
 * (save.c:1283-1328) always zeroes GhostRecord.trackName/unk_12 when building a record
 * from a Ghost, so round-tripping through that path would not reproduce the exact
 * original bytes. Writing the parsed record/data directly keeps export -> import ->
 * export byte-identical for every field, not just the ones a live gameplay save bothers
 * to preserve.
 *
 * ROUND TRIP: exporting the current SRAM ghost and immediately re-importing that exact
 * file reproduces the SRAM slot byte-for-byte (same checksum-valid GhostRecord +
 * GhostData), because both directions move the same 0x3FC0-byte payload verbatim through
 * Save_Read* and Save_Write* -- no field is renamed, dropped, or defaulted in between.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Pass to gdx_ghost_export() to export whatever course the single SRAM ghost slot
 * currently holds, regardless of which course that is. Any other value must match the
 * slot's actual course (see Save_LoadGhostInfo's GhostInfo.courseIndex) or the export
 * fails with GDX_GHOST_ERR_COURSE_MISMATCH -- this lets course-scoped UI (e.g. a
 * Practice-tab "export this course's ghost" button) fail safely instead of silently
 * exporting a different course's ghost when the player didn't realize the slot held one.
 */
#define GDX_GHOST_ANY_COURSE (-1)

/* Return codes. 0 is success; all error codes are negative. */
#define GDX_GHOST_OK 0
#define GDX_GHOST_ERR_BAD_ARGS (-1)          /* NULL/empty path */
#define GDX_GHOST_ERR_IO (-2)                /* file open/read/write/alloc failure */
#define GDX_GHOST_ERR_BAD_MAGIC (-3)         /* not a "GDG1" file */
#define GDX_GHOST_ERR_BAD_VERSION (-4)       /* unsupported/future container version */
#define GDX_GHOST_ERR_BAD_SIZE (-5)          /* payloadSize header field is wrong */
#define GDX_GHOST_ERR_BAD_CONTAINER_CRC (-6) /* file corrupted/truncated in transit */
#define GDX_GHOST_ERR_BAD_CHECKSUM (-7)      /* the game's own record/data checksum failed */
#define GDX_GHOST_ERR_BAD_COURSE (-8)        /* course out of range, or header/payload disagree */
#define GDX_GHOST_ERR_BAD_GHOST_TYPE (-9)    /* ghostType is GHOST_NONE or out of range */
#define GDX_GHOST_ERR_NO_GHOST (-10)         /* export: SRAM slot is empty */
#define GDX_GHOST_ERR_COURSE_MISMATCH (-11)  /* export: wrong course requested; import:
                                                 slot already holds a DIFFERENT course's
                                                 ghost -- never silently overwritten */

/* Suggested default file name for the Practice tab's Export/Import buttons (matches the
 * "ghost_export.gdg next to the exe" default from the feature's design notes). Callers
 * are free to use any path (e.g. from a save-file dialog); this is only a convenience
 * default, not a requirement -- gdx_ghost_export/gdx_ghost_import accept any path. */
#define GDX_GHOST_DEFAULT_FILENAME "ghost_export.gdg"

/* Exports the single SRAM ghost slot to a .gdg file at `path` (created/overwritten).
 *
 * `courseIndex` is either GDX_GHOST_ANY_COURSE (export whatever course is currently
 * saved) or a specific course index that must match the saved ghost's course.
 *
 * Returns GDX_GHOST_OK (0) on success, or a negative GDX_GHOST_ERR_* code. On any
 * failure no file is left behind (a partially-written file is removed) and nothing in
 * SRAM/fzerox.sav is modified -- export is read-only with respect to the save.
 */
int gdx_ghost_export(int courseIndex, const char* path);

/* Imports a .gdg file at `path` and installs it into the single SRAM ghost slot.
 *
 * Returns GDX_GHOST_OK (0) on success, or a negative GDX_GHOST_ERR_* code. On any
 * failure the SRAM ghost slot and fzerox.sav are left completely untouched -- see the
 * VALIDATION and OVERWRITE POLICY sections above for exactly what is checked and when a
 * course conflict is refused rather than silently overwritten.
 */
int gdx_ghost_import(const char* path);

/* Convenience helper: fills `outPath` (a UTF-8/ANSI, NUL-terminated path, at most
 * `outCap` bytes including the terminator) with a default .gdg path -- on Windows, the
 * executable's own directory plus GDX_GHOST_DEFAULT_FILENAME (mirrors port/sram_buffer.
 * cpp's gdx_sram_path pattern for fzerox.sav); on other hosts, just the bare relative
 * filename (a CWD-relative fallback -- this port's persistence is Windows-only so far,
 * matching sram_buffer.cpp's own caveat).
 *
 * Returns 1 on success, 0 if `outPath`/`outCap` are invalid or the path would not fit.
 * This is purely a convenience for callers (e.g. the Practice tab's default Export/
 * Import path) -- gdx_ghost_export/gdx_ghost_import never call it themselves and work
 * with any caller-supplied path.
 */
int gdx_ghost_default_path(char* outPath, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDIFFUSER_GDX_GHOST_IO_H */
