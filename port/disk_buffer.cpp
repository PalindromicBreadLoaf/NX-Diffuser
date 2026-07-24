// G-Diffuser — 64DD disk image loader (Expansion Kit).
// Host-side counterpart of port/n64_leo.c: owns the CRT file I/O the game
// target cannot include. Mirrors rom_buffer.cpp's role for the cartridge.
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_extract_launch.h" // R8: disk deletion-gate helpers (sidecar disk_sha256 + SHA-256 over memory)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h> // readlink
#endif

extern "C" {
unsigned char* gdx_disk_buffer = nullptr;
unsigned int gdx_disk_size = 0;

// 64DD IPL/drive ROM image (user-supplied). Holds the built-in kanji/ANK font
// the EK text renderers DMA from (LeoGetKAdr/LeoGetAAdr + DDROM_FONT_START).
unsigned char* gdx_ddipl_buffer = nullptr;
unsigned int gdx_ddipl_size = 0;

void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize);
// Durable disk-save sidecar (port/disk_savefile.cpp). init computes the pristine
// fingerprint and loads any existing sidecar; apply replays saved dirty ranges
// over the freshly loaded image. Declared here (host TU boundary) rather than via
// the decomp side; see disk_savefile.h.
void gdx_disk_save_init(const char* diskName, const unsigned char* pristine, unsigned int size);
void gdx_disk_save_apply(unsigned char* buffer);
// Post-fill fixup for the fan-translated EK disk's re-authored Create-Machine
// label sub-block (port/gdx_ek_disk_overrides.c). Overwrites three garbled I8
// heading/caption glyphs with authored English ones. No-op for Course Edit
// (its icons decode correctly at the generated offsets — see that file).
void gdx_ek_disk_overrides_apply(void);
void gdx_leo_on_disk_loaded(const unsigned char* disk);

// 64DD boot-logo source texture (decomp/assets/yaml/jp/ek/boot_logo.yaml,
// D_80769DF0, 136x39 RGBA16). Zero-filled by the EK asset generator at build
// time; gdx_ek_assets_fill() below is what actually copies real pixels into
// it from the loaded disk image. sys_main.c's func_806F33D0 blits this buffer
// straight into the VI framebuffer with no RDP task (see the PORT note in
// n64_gfx_bridge.cpp gdx_vi_present_fallback). Declared here (not in a
// decomp header) so this host TU can read it for the boot-logo evidence log
// below without pulling in decomp headers.
extern unsigned short D_80769DF0[];

// Forward declarations of the path helpers defined lower in this TU (used by the raw IPL fallback).
static void gdx_dir_of(const char* path, char* outDir, size_t outSize);
static void gdx_exe_dir(char* outDir, size_t outSize);

// Raw archive-file reader (port/AssetLoader.cpp): copies min(fileSize, outSize) bytes of a mounted
// o2r entry into `out`, returns 1 on success. Used by the archive-first IPL path below to pull the
// pre-sliced font block out of n64ddipl.o2r without touching the IPL ROM file.
extern "C" int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

// R3 IPL geometry (frozen contract C-R3.1/C-R3.2). DDROM_FONT_START is where the drive-ROM font
// block begins; the port allocates a fixed 0x140000-byte buffer, zero-fills [0, 0xA0000), and copies
// the font block at 0xA0000 so every consumer's guard (fontAddr >= 0xA0000 &&
// fontAddr + 0x80 <= gdx_ddipl_size) and LeoGetKAdr's arithmetic hold unchanged.
#define GDX_DDROM_FONT_START 0xA0000u
#define GDX_DDIPL_LOGICAL_SIZE 0x140000u
#define GDX_DDIPL_FONT_BLOCK_BYTES (GDX_DDIPL_LOGICAL_SIZE - GDX_DDROM_FONT_START) // 0xA0000

// C-R3.2 archive-first: when n64ddipl.o2r is mounted, allocate the 0x140000 logical image, zero-fill
// the low region, and copy the archived font block to 0xA0000. Returns true on success. The archived
// slice is already byte-order-normalized big-endian (the gdx-extract `ipl` step normalized it), so no
// swap happens here. Absence of the archive returns false and the raw-file fallback below carries.
static bool gdx_ddipl_load_from_archive(void) {
    unsigned char* buf = static_cast<unsigned char*>(malloc(GDX_DDIPL_LOGICAL_SIZE));
    if (buf == nullptr) {
        return false;
    }
    memset(buf, 0, GDX_DDIPL_LOGICAL_SIZE);
    size_t copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes("ipl/font_block", buf + GDX_DDROM_FONT_START,
                                        GDX_DDIPL_FONT_BLOCK_BYTES, &copied) ||
        copied != GDX_DDIPL_FONT_BLOCK_BYTES) {
        free(buf);
        return false;
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = GDX_DDIPL_LOGICAL_SIZE;
    gdx_port_logf("[leo] 64DD IPL font block served from n64ddipl.o2r (%u bytes at 0x%X)\n",
                  GDX_DDIPL_FONT_BLOCK_BYTES, GDX_DDROM_FONT_START);
    return true;
}

// Best-effort lookup of the firstboot-recorded IPL path (Game.DdIplPath in gdx_firstboot.cfg). In
// installed mode the process CWD is the data dir; in the dev/portable layout the cfg sits next to the
// exe. Reads the first cfg found. Returns true and fills `outPath` when the key is present.
static bool gdx_firstboot_ipl_path(char* outPath, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    outPath[0] = '\0';
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    const char* dirs[] = { "", exeDir }; // "" == current working directory
    for (const char* dir : dirs) {
        char cfgPath[1200];
        snprintf(cfgPath, sizeof(cfgPath), "%sgdx_firstboot.cfg", dir);
        FILE* cf = fopen(cfgPath, "rb");
        if (cf == nullptr) {
            continue;
        }
        char line[2048];
        bool found = false;
        while (fgets(line, sizeof(line), cf) != nullptr) {
            // Strip trailing CR/LF.
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            const char* kv = "Game.DdIplPath=";
            const size_t kvLen = strlen(kv);
            if (strncmp(line, kv, kvLen) == 0 && line[kvLen] != '\0') {
                strncpy(outPath, line + kvLen, outSize - 1);
                outPath[outSize - 1] = '\0';
                found = true;
                break;
            }
        }
        fclose(cf);
        if (found) {
            return true;
        }
    }
    return false;
}

// Opens the IPL dump by trying, in order: the firstboot-recorded path, then the canonical
// "N64DDIPLROM.n64" next to (a) the exe, (b) the chosen ROM, (c) the current working directory. This
// replaces the old bare CWD-relative fopen (C-R3.4 / RELEASE_HYGIENE): a launch whose CWD is not the
// data dir no longer silently loses the drive-ROM font. Returns an open FILE* (caller closes) or null.
static FILE* gdx_ddipl_open_raw(char* chosenPath, size_t chosenSize) {
    if (chosenSize > 0) {
        chosenPath[0] = '\0';
    }
    // (0) firstboot-recorded absolute path.
    char recorded[1024] = {};
    if (gdx_firstboot_ipl_path(recorded, sizeof(recorded)) && recorded[0] != '\0') {
        FILE* f = fopen(recorded, "rb");
        if (f != nullptr) {
            if (chosenSize > 0) {
                strncpy(chosenPath, recorded, chosenSize - 1);
                chosenPath[chosenSize - 1] = '\0';
            }
            return f;
        }
    }
    // (1) exe dir, (2) next to the chosen ROM, (3) current working directory.
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    const char* dirs[] = { exeDir, romDir, "" };
    for (const char* dir : dirs) {
        char path[1200];
        snprintf(path, sizeof(path), "%sN64DDIPLROM.n64", dir);
        FILE* f = fopen(path, "rb");
        if (f != nullptr) {
            if (chosenSize > 0) {
                strncpy(chosenPath, path, chosenSize - 1);
                chosenPath[chosenSize - 1] = '\0';
            }
            return f;
        }
    }
    return nullptr;
}

// Raw-file fallback (retained until R4): loads N64DDIPLROM.n64 and normalizes it to native
// big-endian byte order (dumps circulate as z64/BE, v64/16-bit-swapped, or n64/32-bit-LE; detect by
// the first byte of the PI header, which is 0x80 in native order).
static void gdx_ddipl_load_from_raw(void) {
    char chosen[1200] = {};
    FILE* f = gdx_ddipl_open_raw(chosen, sizeof(chosen));
    if (f == nullptr) {
        gdx_port_logf("[leo] no N64DDIPLROM.n64 (and no n64ddipl.o2r); drive-ROM font stays blank\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x100000) { // sanity: IPL ROM dumps are 4MB
        fclose(f);
        gdx_port_logf("[leo] %s too small (%ld bytes); ignored\n", chosen, sz);
        return;
    }
    unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
    if (buf == nullptr || fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    if (buf[0] == 0x80) {
        // native big-endian
    } else if (buf[1] == 0x80) {
        for (long i = 0; i + 1 < sz; i += 2) { // v64: swap 16-bit pairs
            unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
    } else if (buf[3] == 0x80) {
        for (long i = 0; i + 3 < sz; i += 4) { // n64: reverse 32-bit words
            unsigned char t0 = buf[i], t1 = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1; buf[i + 3] = t0;
        }
    } else {
        gdx_port_logf("[leo] %s: unrecognized byte order (first bytes %02X %02X %02X %02X); using as-is\n",
                      chosen, buf[0], buf[1], buf[2], buf[3]);
    }
    gdx_ddipl_buffer = buf;
    gdx_ddipl_size = static_cast<unsigned int>(sz);
    gdx_port_logf("[leo] 64DD IPL ROM loaded from %s (%ld bytes)\n", chosen, sz);
}

// Provisioning order (C-R3.2): the dedicated archive n64ddipl.o2r first (so a completed setup can
// delete the IPL ROM file), then the raw-IPL-file fallback (retained until R4's soak rule).
static void gdx_ddipl_load(void) {
    if (gdx_ddipl_buffer != nullptr) {
        return;
    }
    if (gdx_ddipl_load_from_archive()) {
        return;
    }
    gdx_ddipl_load_from_raw();
}

// Best-effort check of whether the loaded cartridge ROM is the Japanese
// release, from the standard N64 ROM header country-code byte (offset 0x3E).
// Only meaningful for a big-endian (.z64) image -- confirmed via the z64
// magic word at offset 0 (0x80 0x37 0x12 0x40). gdx_init_rom() does not
// normalize v64 (byte-swapped) or n64 (word-swapped) dumps today, so for
// those we can't read the header reliably and fall back to "not confidently
// Japanese", which keeps today's default disk preference (translated-first).
static bool gdx_rom_is_japanese(void) {
    if (gdx_rom_buffer == nullptr || gdx_rom_size < 0x40) {
        return false;
    }
    if (gdx_rom_buffer[0] != 0x80 || gdx_rom_buffer[1] != 0x37) {
        return false;
    }
    return gdx_rom_buffer[0x3E] == 'J';
}

// Directory (including trailing separator) of a file path, or "" if the path
// has no directory component. Handles both '/' and '\\' since ROM paths can
// come from a Windows file picker (backslash) or a hand-typed FZEROX_ROM
// value (either).
static void gdx_dir_of(const char* path, char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (path == nullptr || path[0] == '\0' || outSize == 0) {
        return;
    }
    const char* slash = strrchr(path, '\\');
    const char* fwdSlash = strrchr(path, '/');
    if (fwdSlash != nullptr && (slash == nullptr || fwdSlash > slash)) {
        slash = fwdSlash;
    }
    if (slash == nullptr) {
        return;
    }
    size_t len = static_cast<size_t>(slash - path) + 1; // keep the separator
    if (len >= outSize) {
        len = outSize - 1;
    }
    memcpy(outDir, path, len);
    outDir[len] = '\0';
}

static void gdx_exe_dir(char* outDir, size_t outSize) {
    outDir[0] = '\0';
    if (outSize == 0) {
        return;
    }
#ifdef _WIN32
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return;
    }
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) {
        return;
    }
    slash[1] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, outDir, static_cast<int>(outSize), nullptr, nullptr);
#else
    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n <= 0) {
        return;
    }
    exePath[n] = '\0';
    char* slash = strrchr(exePath, '/');
    if (slash == nullptr) {
        return;
    }
    slash[1] = '\0'; // keep trailing separator
    if (strlen(exePath) >= outSize) {
        return;
    }
    strcpy(outDir, exePath);
#endif
}

// R8 Step 1: canonical retail/translated EK image size and the .gdd save key for archive-sourced
// loads. The R7 managed copy is always kept under this leaf name (gdx_firstboot.cpp copies every
// source to it), so an archive built from that copy replays saves under the exact same key the
// managed-copy file path would use -- ".gdd keying (canonical leaf name) unchanged" (R8 invariant).
#define GDX_DISK_EXACT_BYTES 64931840u
static const char* const kDiskArchiveSaveKey = "baserom.translated.ek.ndd";

// Deletion-gate verdict. 1 ONLY after a boot both (a) reconstructed the disk from fzerox-disk.o2r and
// (b) verified SHA-256(reconstructed image) == the R7 managed-copy sha. Read by the Data & Files panel
// via gdx_disk_archive_verified(); the panel offers deletion only on a passed verdict. No code ever
// deletes user files -- this is a UX gate, not an action.
static int s_diskArchiveVerified = 0;

// Common post-load tail, shared by the archive-first branch and the raw-file/managed-copy branch.
// Assumes gdx_disk_buffer/gdx_disk_size are already set to the PRISTINE image bytes. Runs the leo
// disk-loaded hook, the EK asset fill, the translated-disk label overrides, the boot-logo texel swap,
// IPL provisioning, then the durable .gdd save init + replay. `diskName` keys the .gdd sidecar
// (canonical leaf name only, never a path). `sourceLabel` is logged as "[leo] disk source:".
static void gdx_disk_finalize(const char* diskName, const char* sourceLabel) {
    // R7 (C-R7.2): explicit provenance line, stable for a "disk source:" log grep (archive|managed|original).
    gdx_port_logf("[leo] disk source: %s\n", sourceLabel);
    gdx_leo_on_disk_loaded(gdx_disk_buffer);
    gdx_ek_assets_fill(gdx_disk_buffer, static_cast<unsigned long long>(gdx_disk_size));
    gdx_ek_disk_overrides_apply();

    /* Host byte order for the boot-logo texels (2026-07-09): the fill above copies raw BIG-ENDIAN
       disk bytes, but func_806F33D0 (sys_main.c) CPU-blits these u16s into a VI framebuffer that
       everything else treats as host-order. Swap once here, at the only fill site; the blit is this
       texture's only consumer (no GFX-task path reads it as big-endian). This is a decoded-TEXTURE
       fixup, NOT a disk-image normalization -- it runs identically on the archive and file paths, and
       the disk buffer itself is never byte-swapped (which is why the archive stores it verbatim). */
    for (unsigned int i = 0; i < 5304; i++) {
        const unsigned short v = D_80769DF0[i];
        D_80769DF0[i] = static_cast<unsigned short>((v >> 8) | (v << 8));
    }

    gdx_ddipl_load();

    // Durable disk save: gdx_disk_buffer is still the PRISTINE image here (the calls above take it as
    // const / patch decoded C arrays, not the disk buffer), so its CRC64 fingerprint is the pristine
    // one. init records that fingerprint + loads any matching sidecar; apply replays saved dirty ranges.
    gdx_disk_save_init(diskName, gdx_disk_buffer, gdx_disk_size);
    gdx_disk_save_apply(gdx_disk_buffer);
}

// R8 Step 1 archive-first: when fzerox-disk.o2r is mounted and its disk/image inflates to exactly
// GDX_DISK_EXACT_BYTES, use it as the in-memory image source. The archive stores the disk bytes
// VERBATIM (the loader never byte-swaps the disk buffer -- see gdx_disk_finalize), so archive-sourced
// and file-sourced images are byte-identical: the existing .gdd replay, gdx_ek_assets_fill, and
// byte-order handling all run unchanged, and there is no swap step to bypass. Returns 1 on success
// (buffer installed + finalized), 0 to fall through to the managed-copy/raw-file search.
static int gdx_disk_load_from_archive(void) {
    unsigned char* buf = static_cast<unsigned char*>(malloc(GDX_DISK_EXACT_BYTES));
    if (buf == nullptr) {
        return 0;
    }
    size_t copied = 0;
    if (!GDiffuser_LoadArchiveFileBytes("disk/image", buf, GDX_DISK_EXACT_BYTES, &copied) ||
        copied != GDX_DISK_EXACT_BYTES) {
        // No archive mounted, or disk/image did not inflate to the exact size -- fall through.
        free(buf);
        return 0;
    }

    // ── Deletion gate ────────────────────────────────────────────────────────────────────────────
    // SHA-256 of the reconstructed image must equal the R7 managed-copy sha (sidecar disk_sha256)
    // before the Data & Files panel may EVER offer deletion. Compute on the PRISTINE bytes now, before
    // gdx_disk_save_apply (inside finalize) replays saves over them. The recorded value is preferred;
    // if the sidecar has none, hash the managed copy file once as a fallback. A missing managed copy or
    // any mismatch leaves the verdict false -- the gate never suggests deletion on unproven bytes.
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    std::string archiveSha = gdx::GdxExtractSha256Bytes(buf, static_cast<unsigned long long>(GDX_DISK_EXACT_BYTES));
    std::string recorded = gdx::GdxExtractRecordedDiskSha256(exeDir);
    if (recorded.empty() && exeDir[0] != '\0') {
        char managedPath[1200];
        snprintf(managedPath, sizeof(managedPath), "%smedia/%s", exeDir, kDiskArchiveSaveKey);
        recorded = gdx::GdxExtractFileSha256(managedPath);
    }
    s_diskArchiveVerified = (!recorded.empty() && archiveSha == recorded) ? 1 : 0;
    if (s_diskArchiveVerified) {
        gdx_port_logf("[leo] disk archive verified byte-identical to the managed copy (SHA-256 %s); "
                      "original .ndd and managed copy are deletable\n",
                      archiveSha.c_str());
    } else {
        gdx_port_logf("[leo] disk archive NOT verified (reconstructed %s vs recorded %s); deletion "
                      "stays gated\n",
                      archiveSha.empty() ? "(none)" : archiveSha.c_str(),
                      recorded.empty() ? "(none)" : recorded.c_str());
    }

    gdx_disk_buffer = buf;
    gdx_disk_size = GDX_DISK_EXACT_BYTES;
    gdx_port_logf("[leo] disk image reconstructed from fzerox-disk.o2r (%u bytes)\n", GDX_DISK_EXACT_BYTES);
    gdx_disk_finalize(kDiskArchiveSaveKey, "archive");
    return 1;
}

// R8 Step 1: deletion-gate verdict for the Data & Files panel. 1 iff this boot reconstructed the disk
// from the archive AND proved byte-identity against the managed copy. Never triggers any deletion.
int gdx_disk_archive_verified(void) {
    return s_diskArchiveVerified;
}

int gdx_disk_load(void) {
    if (gdx_disk_buffer != nullptr) {
        return 1;
    }

    // R8 Step 1: archive-first. A mounted, exact-size fzerox-disk.o2r reconstructs the image (and runs
    // the deletion gate); the managed-copy/raw-file search below is the unchanged fallback.
    if (gdx_disk_load_from_archive()) {
        return 1;
    }

    // Disk name preference order: the fan-translated English disk
    // (LuigiBlood/Zoinkity, 64DD.org) keeps the retail disk layout, so
    // loading it instead of the JP dump delivers English EK text/font
    // assets through the exact same offsets and MFS reads -- no extraction
    // step needed. That's the right default and the fallback whenever the
    // loaded ROM's region can't be determined. For a confirmed Japanese
    // cartridge ROM, prefer the untranslated JP disk so on-disk text
    // matches the cartridge's language.
    const bool jpRom = gdx_rom_is_japanese();
    static const char* const kUsPreferredNames[] = {
        "baserom.translated.ek.ndd",
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
    };
    static const char* const kJpPreferredNames[] = {
        "baserom.jp.ek.ndd",
        "baserom.jp.disk",
        "baserom.translated.ek.ndd",
    };
    const char* const* diskNames = jpRom ? kJpPreferredNames : kUsPreferredNames;
    const size_t diskNameCount = 3;

    // Search location order (R7 -- disk internalization): (0) the managed copy under
    // <exeDir>/media -- a byte-identical copy gdx_firstboot.cpp creates from the user's original
    // disk at setup time, kept under the SAME canonical leaf name, so the .gdd save key (which
    // derives from the leaf name only, never a path -- see disk_savefile.cpp:381) is unaffected by
    // preferring it -- then (1) next to the chosen ROM -- so multiple installs/folders with
    // different .ndd files never cross-pollinate -- then (2) the exe directory, then (3) the
    // process's current working directory (legacy behavior, kept as a last resort for scripted/dev
    // launches that rely on CWD rather than exe-relative paths).
    char romDir[1024] = {};
    gdx_dir_of(gdx_rom_path, romDir, sizeof(romDir));
    char exeDir[1024] = {};
    gdx_exe_dir(exeDir, sizeof(exeDir));
    char mediaDir[1040] = {};
    if (exeDir[0] != '\0') {
        snprintf(mediaDir, sizeof(mediaDir), "%smedia/", exeDir);
    }

    struct SearchLocation {
        const char* dir;
        const char* why;
        bool managed;
    };
    const SearchLocation searchLocations[] = {
        { mediaDir, "managed copy (media/)", true },
        { romDir, "next to chosen ROM", false },
        { exeDir, "exe directory", false },
        { "", "current directory", false },
    };

    for (const SearchLocation& loc : searchLocations) {
        for (size_t i = 0; i < diskNameCount; i++) {
            char path[1200];
            snprintf(path, sizeof(path), "%s%s", loc.dir, diskNames[i]);

            FILE* f = fopen(path, "rb");
            if (f == nullptr) {
                continue;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                continue;
            }
            /* Codebase-audit P2: gdx_leo_on_disk_loaded and LeoReadDiskID read the
               LBA-14 disk ID and system area at fixed offsets with no size guard,
               and gdx_ek_assets_fill walks generated diskOffset tables. A truncated
               .ndd would OOB-read immediately. Retail 64DD images are ~64.9 MB;
               require a sane minimum before handing the buffer to those consumers. */
            if (sz < (long)(60u * 1024u * 1024u)) {
                gdx_port_logf("[leo] REJECTED %s: %ld bytes (truncated .ndd image, need ~64.9 MB)\n", path, sz);
                fclose(f);
                continue;
            }
            unsigned char* buf = static_cast<unsigned char*>(malloc(static_cast<size_t>(sz)));
            if (buf == nullptr) {
                fclose(f);
                return 0;
            }
            if (fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
                free(buf);
                fclose(f);
                continue;
            }
            fclose(f);
            gdx_disk_buffer = buf;
            gdx_disk_size = static_cast<unsigned int>(sz);
            gdx_port_logf("[leo] disk image loaded: %s (%ld bytes) -- picked from %s, region=%s\n", path, sz,
                          loc.why, jpRom ? "JP (matches cartridge ROM)" : "US/unknown (default preference)");
            // Managed-copy/raw-file load: no archive reconstruction happened this boot, so the deletion
            // gate stays unproven (s_diskArchiveVerified remains 0) and the disk is NOT marked deletable.
            gdx_disk_finalize(diskNames[i], loc.managed ? "managed" : "original");
            return 1;
        }
    }

    static bool sWarned = false;
    if (!sWarned) {
        sWarned = true;
        gdx_port_logf("[leo] no disk image found; Expansion Kit modes stay disabled\n");
    }
    return 0;
}
}
