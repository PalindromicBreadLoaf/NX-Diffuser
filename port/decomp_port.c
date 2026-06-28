// G-Diffuser — decomp-side port subsystems (Slice 4c).
// Compiled WITH the decomp's headers/flags (part of the gdiffuser_game target), so it uses
// the real game types. Provides port reimplementations / placeholders for symbols that lived
// in N64-platform files excluded from the host build (segment system, save, fixed-address
// data). Placeholders let the exe LINK and boot; real backing (resource system, LUS save)
// comes next.

#include "global.h"
#include "fzx_course.h"
#include "fzx_game.h"
#include "n64_rdram.h"

/* port_log.h pulls in <stdio.h> which clashes with the decomp's libc/stdint.h.
   Use gdx_ck (defined in n64_sched.c, which CAN include stdio.h) for logging. */
extern void gdx_ck(const char* s);
extern void gdx_seg_log(const char* kind, int seg, uintptr_t raw, void* resolved);
extern void gdx_addr_log(const char* kind, uintptr_t raw, void* resolved);
extern void* gdx_host_calloc(size_t count, size_t size);
extern void  gdx_host_exit(int status);
extern void  gdx_host_abort(void);
extern void* gdx_resolve_registered_host_address(unsigned int addr);
extern void* gdx_resolve_module_host_address(unsigned int addr);
extern void* gdx_ensure_asset_segment_for_symbol(unsigned int symLow32, unsigned int* outOffset);
extern int   gdx_load_venue_texture_segment(int venue);
extern s32   gGameMode;

extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;
extern unk_80225800 D_80225800_2;

// ---- RDRAM host buffer globals ----------------------------------------------
// gdx_rdram: single 16MB contiguous buffer allocated by gdx_rdram_init().
// All N64 physical addresses resolve to gdx_rdram + phys.
// gdx_gfxpool: pointer to the GfxPool D_1000000 object (alias, not RDRAM-resident).
// gdx_rdram_bump: byte offset of the next free arena byte (carves upward).
// gdx_rdram_arena_start: first arena byte after the GfxPool reservation.

unsigned char* gdx_rdram          = NULL;
static size_t  gdx_rdram_bump     = 0;
size_t         gdx_rdram_arena_start = 0;
GfxPool*       gdx_gfxpool        = NULL;

static size_t Gdx_RomOffset(u32 addr) {
    u32 phys = addr & 0x1FFFFFFF;
    return (phys >= 0x10000000) ? (size_t)(phys - 0x10000000) : (size_t)phys;
}

// ---- RDRAM init + bump allocator -------------------------------------------

extern void gdx_register_host_range(void* ptr, size_t size); // defined in n64_gfx_bridge.cpp
extern GfxPool D_1000000; // defined below; forward-declared here so gdx_rdram_init() can reference it
extern GfxPool D_8024DCE0[2];

void gdx_rdram_init(void) {
    gdx_rdram = (unsigned char*)gdx_host_calloc(1, GDX_RDRAM_SIZE);
    if (gdx_rdram == NULL) {
        gdx_ck("[rdram] FATAL: failed to allocate 8MB RDRAM buffer");
        gdx_host_exit(1);
    }

    // Arena starts after the GfxPool reservation, 16-byte aligned.
    gdx_rdram_arena_start = GDX_RDRAM_GFXPOOL_OFFSET +
                            ((sizeof(GfxPool) + 15u) & ~(size_t)15u);
    gdx_rdram_bump        = gdx_rdram_arena_start;

    // D_1000000 is a linker-symbol BSS global; point gdx_gfxpool at it.
    // The GfxPool stays as a host BSS allocation so all extern GfxPool D_1000000
    // declarations in the decomp source files link correctly without modification.
    gdx_gfxpool = &D_1000000;

    // Register the whole RDRAM buffer once — covers all future arena allocs.
    gdx_register_host_range(gdx_rdram, GDX_RDRAM_SIZE);
    gdx_register_host_range(&D_1000000, sizeof(D_1000000));
    gdx_register_host_range(D_8024DCE0, sizeof(D_8024DCE0));

    {
        extern void gdx_cki(const char*, int);
        extern void gdx_ckp(const char*, void*);
        gdx_ckp("[rdram] base", (void*)gdx_rdram);
        gdx_cki("[rdram] arena_start", (int)gdx_rdram_arena_start);
        gdx_cki("[rdram] sizeof GfxPool", (int)sizeof(GfxPool));
    }

    gdx_ck("[rdram] 8MB buffer initialized");
}

void* gdx_rdram_alloc_raw(size_t size, size_t align) {
    size_t base = (gdx_rdram_bump + (align - 1u)) & ~(align - 1u);
    if (base + size > GDX_RDRAM_SIZE) {
        gdx_ck("[rdram] FATAL: arena exhausted");
        gdx_host_abort();
    }
    gdx_rdram_bump = base + size;
    return gdx_rdram + base;
}

// ---- Physical <-> Virtual address translation (PORT) -----------------------
// physicaltovirtual.c and virtualtophysical.c are excluded from the CMake build
// (they contain N64-platform implementations). Port implementations live here
// where both gdx_rdram and gdx_rom_buffer are visible.

#ifdef PORT
void* osPhysicalToVirtual(u32 addr) {
    u32 phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x10000000u) {
        // Cart ROM region: map into the host ROM buffer.
        if (gdx_rom_buffer != NULL) {
            return gdx_rom_buffer + (phys - 0x10000000u);
        }
        return gdx_rdram; // safe non-NULL fallback
    }
    // RDRAM region.
    return gdx_rdram + phys;
}

u32 osVirtualToPhysical(void* vaddr) {
    unsigned char* c = (unsigned char*)vaddr;
    // RDRAM-resident pointer: return offset from base.
    if (gdx_rdram != NULL && c >= gdx_rdram && c < gdx_rdram + GDX_RDRAM_SIZE) {
        return (u32)(c - gdx_rdram);
    }
    /* Host/BSS pointers cannot fit in libultra's u32 physical-address return.
       Return a low32 token, then Gdx_ResolvePortAddress reconstructs it via
       registered host ranges or the EXE module range before storing gSegments[]. */
    return (u32)(uintptr_t)vaddr;
}
#endif

u32 gdx_rom_read32(u32 addr) {
    size_t off = Gdx_RomOffset(addr);
    if (gdx_rom_buffer == NULL || off + sizeof(u32) > gdx_rom_size) {
        return 0;
    }

    return ((u32)gdx_rom_buffer[off + 0] << 24) |
           ((u32)gdx_rom_buffer[off + 1] << 16) |
           ((u32)gdx_rom_buffer[off + 2] << 8)  |
           ((u32)gdx_rom_buffer[off + 3] << 0);
}

u32 gdx_io_read(u32 addr) {
    (void)addr;
    return 0;
}

void gdx_io_write(u32 addr, u32 data) {
    (void)addr;
    (void)data;
}

// ---- Segment system (port reimplementation) --------------------------------
// N64 mapped 16 graphics segments to RDRAM; the game addresses assets as (segment<<24|offset).
// On host we store REAL pointers per segment (populated as assets load) and translate directly
// — no N64 KSEG0 (PHYS_TO_K0) mapping.
unsigned long long gSegments[16];

static void* Gdx_ResolvePortAddress(uintptr_t addr) {
    static int resolveLogs = 0;
    unsigned long long wideAddr = (unsigned long long)addr;
    unsigned int raw;
    unsigned int assetOffset = 0;
    void* assetBase;
    void* registered;
    void* moduleHost;

    if (addr == 0) {
        return NULL;
    }

    /* If a PORT call path already preserved a full host pointer, keep it.  The
       low32 reconstruction below is only for legacy u32 paths such as
       osVirtualToPhysical() and display-list command words. */
    if (wideAddr > 0xFFFFFFFFULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("full", addr, (void*)addr);
        }
        return (void*)addr;
    }

    raw = (unsigned int)addr;

    /* LinkStubs.c can only provide a one-byte marker for the original linker
       segment symbol. Segment 2, however, addresses the real host-compiled BSS
       object beginning at D_80225800_2 (not that marker). Bind the start token
       explicitly so segmented pointers such as 0x02000000 resolve to the
       matrix/context storage they reference. */
    if (raw == (unsigned int)(uintptr_t)SEGMENT_VRAM_START(unk_bss_segment)) {
        void* resolved = &D_80225800_2;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("unk-bss", addr, resolved);
        }
        return resolved;
    }

    assetBase = gdx_ensure_asset_segment_for_symbol(raw, &assetOffset);
    if (assetBase != NULL) {
        void* resolved = (u8*)assetBase + assetOffset;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("asset", addr, resolved);
        }
        return resolved;
    }

    registered = gdx_resolve_registered_host_address(raw);
    if (registered != NULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("registered", addr, registered);
        }
        return registered;
    }

    moduleHost = gdx_resolve_module_host_address(raw);
    if (moduleHost != NULL) {
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("module", addr, moduleHost);
        }
        return moduleHost;
    }

    if ((raw >= 0x80000000u) && (raw <= 0xBFFFFFFFu)) {
        unsigned int phys = raw & 0x1FFFFFFFu;
        if ((gdx_rdram != NULL) && (phys < GDX_RDRAM_SIZE)) {
            void* resolved = gdx_rdram + phys;
            if (resolveLogs < 12) {
                resolveLogs++;
                gdx_addr_log("kseg-rdram", addr, resolved);
            }
            return resolved;
        }
        return NULL;
    }

    if ((gdx_rdram != NULL) && (raw < GDX_RDRAM_SIZE)) {
        void* resolved = gdx_rdram + raw;
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("rdram", addr, resolved);
        }
        return resolved;
    }

    if ((((raw >> 24) & 0xF) < 16) && (gSegments[(raw >> 24) & 0xF] != 0)) {
        void* resolved = (void*)(gSegments[(raw >> 24) & 0xF] + (raw & 0x00FFFFFFu));
        if (resolveLogs < 12) {
            resolveLogs++;
            gdx_addr_log("segmented", addr, resolved);
        }
        return resolved;
    }

    if (resolveLogs < 12) {
        resolveLogs++;
        gdx_addr_log("fallback-low32", addr, (void*)(unsigned long long)raw);
    }
    return (void*)(unsigned long long)raw;
}

void* gdx_segmented_to_host_pointer(uintptr_t segmentedAddr) {
    return Gdx_ResolvePortAddress(segmentedAddr);
}

uintptr_t Segment_SegmentedToVirtual(uintptr_t segmentedAddr) {
    return (uintptr_t)(unsigned long long)Gdx_ResolvePortAddress(segmentedAddr);
}

uintptr_t Segment_SetPhysicalAddress(s32 segment, uintptr_t addr) {
    void* resolved = Gdx_ResolvePortAddress(addr);
    gSegments[segment] = (unsigned long long)resolved;
    gdx_seg_log("SetPhys", segment, addr, resolved);
    return addr;
}

uintptr_t Segment_SetAddress(s32 segment, uintptr_t addr) {
    void* resolved = Gdx_ResolvePortAddress(addr);
    gSegments[segment] = (unsigned long long)resolved;
    gdx_seg_log("SetAddr", segment, addr, resolved);
    return addr;
}

uintptr_t Segment_GetAddress(s32 segment) {
    return (uintptr_t)gSegments[segment];
}

Gfx* Segment_SetTableAddresses(Gfx* gfx) {
    // Emit one gSPSegment per slot so the converted display list carries correct
    // segment bases into the LUS interpreter's mSegmentPointers[]. The adapter's
    // kOpMoveword handler ignores the truncated 32-bit w1 and reads gSegments[]
    // directly, so the full 64-bit host pointers survive the 8→16-byte conversion.
    for (int i = 0; i < 16; i++) {
        gSPSegment(gfx++, i, gSegments[i]);
    }
    return gfx;
}

void Segment_LoadAssets(void) {
    switch (GET_MODE(gGameMode)) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_RECORDS:
        case GAMEMODE_COURSE_EDIT:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_GP_END_CS:
        case GAMEMODE_DEATH_RACE:
            if (!gdx_load_venue_texture_segment(COURSE_CONTEXT()->courseData.venue)) {
                gdx_ck("[segment] venue texture segment load failed");
            }
            break;
        default:
            break;
    }
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
