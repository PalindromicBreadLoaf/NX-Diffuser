#!/usr/bin/env python3
"""G-Diffuser link-stub generator (Slice 4c / R4+R5 bring-up).

Reads the undefined symbols from a top-build.log and emits port/gen/LinkStubs.c defining each
so the executable LINKS. Data-like symbols (segment/overlay/framebuffer linker markers, globals,
microcode blobs) become 1-byte placeholders; the rest become no-op functions returning 0.

These are PLACEHOLDERS to reach a linked binary. Real implementations (segment system, overlay
loader, save/leo/mfs, arena, audio microcode) are R5/R6 — verified on a real desktop runtime.
"""
import glob
import os
import re

import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(REPO, "top-build.log")
OUT = os.path.join(REPO, "port", "gen", "LinkStubs.c")
ASSET_YAML_DIR = os.path.join(REPO, "decomp", "assets", "yaml", "us", "rev0")


def yaml_table_sizes():
    """Real byte sizes for yaml :config: table symbols (e.g. aPositionDigitTexs).

    Game code addresses these as interior pointers (base + index * stride), and
    the runtime asset resolvers match interior references against the symbol's
    range. A 1-byte placeholder makes that range window swallow every stub
    symbol packed after it, so table symbols MUST be defined at their real size.
    Mirrors the table scan in gen_asset_bindings.py.
    """
    sizes = {}
    for path in sorted(glob.glob(os.path.join(ASSET_YAML_DIR, "*.yaml"))):
        with open(path) as f:
            data = yaml.safe_load(f.read()) or {}
        tables = (data.get(":config", {}) or {}).get("tables") or {}
        for table_name, table_val in tables.items():
            if not isinstance(table_val, dict):
                continue
            table_range = table_val.get("range")
            if not isinstance(table_range, (list, tuple)) or len(table_range) < 2:
                continue
            size = int(table_range[1]) - int(table_range[0])
            if size > 0:
                sizes[table_name] = size
    return sizes

# Symbols we implement for real elsewhere (shims.c, decomp_port.c) — never stub these.
EXCLUDE = {
    "Arena_Allocate", "Arena_StartInit", "Arena_DefaultStartInit", "Arena_EndInit",
    # R6: decomp's libultra/os/*.c is now compiled (real N64 cooperative scheduler) — these
    # are DEFINED by the decomp, so they must not be stubbed (would be duplicate symbols).
    "osCreateThread", "osStartThread", "osStopThread", "osDestroyThread", "osYieldThread",
    "osSetThreadPri", "osGetThreadPri", "osGetActiveQueue", "__osGetActiveQueue",
    "osCreateMesgQueue", "osSendMesg", "osRecvMesg", "osJamMesg", "osSetEventMesg",
    "osPhysicalToVirtual", "osVirtualToPhysical", "osGetMemSize", "osMemSize", "osInitialize",
    "osSetGlobalIntMask", "osResetGlobalIntMask",
    # R6: port-provided primitives (port/n64_sched.c context switch + gfx bridge) — we WRITE
    # these, so don't stub them either.
    "__osDispatchThread", "__osEnqueueAndYield", "__osEnqueueThread", "__osPopThread",
    "__osDequeueThread", "__osThreadTail", "__osDisableInt", "__osRestoreInt",
    "__osCleanupThread", "__osGetCurrFaultedThread",
    "osSpTaskStart", "osSpTaskLoad", "osSpTaskStartGo", "osSpTaskYield", "osSpTaskYielded",
    # R6 VI bridge (port/n64_vi.c) — we provide these (libultraship os_vi.cpp is disabled).
    "osViSwapBuffer", "osViGetCurrentFramebuffer", "osViGetNextFramebuffer", "osViSetEvent",
    "osCreateViManager", "osViSetMode", "osViBlack", "osViSetSpecialFeatures",
    "osViSetXScale", "osViSetYScale", "osMemSize",
    # PC has no 64DD medium; shims.c reports that state with the real signature.
    "LeoTestUnitReady",
    # libultra globals the decomp uses as ARRAYS (must be real writable data, not function stubs).
    "osAppNMIBuffer",
    # Save-system slice: decomp/src/overlays/ovl_i2/save.c is now compiled (real
    # cart-SRAM save logic, host-backed via port/sram_buffer.cpp) — these are DEFINED
    # by the decomp there, so they must not be stubbed (would be duplicate symbols).
    "Save_Init", "Save_InitGhost", "Save_Load", "Save_LoadGhost", "Save_LoadGhostInfo",
    "Save_SaveCourseRecordProfiles", "Save_SaveDeathRaceProfiles", "Save_SaveGhost",
    "Save_UpdateCharacterSave", "Save_UpdateCourseCharacterSave", "Save_UpdateCupCompletion",
    "Save_UpdateCupSave", "Sram_Init", "Sram_ReadWrite", "Save_LoadStaffGhostRecord",
    "Save_SaveSettingsProfiles", "D_i2_8010ADE0", "gSettingSoundMode", "gSramPiHandlePtr",
    "func_i2_801017B8", "func_i2_801039BC",
    # EK save/ghost surface also defined for real in save.c now.
    "Save_CalculateGhostRecordChecksum", "Save_CalculateSaveCourseRecordChecksum",
    "Save_ClearCourseRecord", "Save_ClearGhostRecord", "Save_GetDDStaffGhostCompletion",
    "Save_GetDDStaffGhostRecordTime", "Save_InitCourseRecord", "Save_LoadGhostData",
    "Save_ReadGhostData", "Save_SaveGhostData", "Save_SaveGhostRecord",
    "Save_SetDDStaffGhostComplete", "Save_WriteGhostData", "Save_WriteGhostRecord",
    "sDDStaffGhostRecordTimes", "func_i2_800A8CE4", "D_i2_80111848",
}

syms = set()
with open(LOG, encoding="utf-8", errors="ignore") as f:
    for line in f:
        m = re.search(r"undefined symbol: (\S+)", line)
        if m:
            s = m.group(1).strip()
            if s not in EXCLUDE:
                syms.add(s)

# Guard: LinkStubs is generated from a FAILING build log's undefined symbols. If the log is a
# clean build (no undefined symbols), do NOT overwrite — that would wipe the existing stubs. To
# regenerate, first empty/remove LinkStubs.c, build (it will fail), then run this against that log.
if not syms:
    print("gen_link_stubs: no 'undefined symbol' lines in {} — leaving {} unchanged.".format(
        os.path.basename(LOG), os.path.relpath(OUT, REPO)))
    raise SystemExit(0)

# Heuristic: linker-marker / global / blob symbols are DATA; everything else is a function.
DATA_RE = re.compile(
    r"(_VRAM(_END)?$|_ROM_(START|END)$|_BSS_(START|END)$|_DATA_(START|END|SIZE)$"
    r"|_TEXT_(START|END)$|_RODATA_END$|^ovl_i|^framebuffer|^D_[0-9A-Fa-f]|^g[A-Z]|^s[A-Z]"
    r"|^a[A-Z]|fifo(Text|Data)(Start|End)$|^rspboot|^leoBootID$|qnan|^unk_|osViMode|^osTvType$)"
)

data = sorted(s for s in syms if DATA_RE.search(s))
funcs = sorted(s for s in syms if not DATA_RE.search(s))

table_sizes = yaml_table_sizes()

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    f.write("// AUTO-GENERATED by tools/gen_link_stubs.py. Placeholder defs for unported symbols.\n")
    f.write("// Data = 1-byte buffers (yaml table symbols at their real range size);\n")
    f.write("// functions = no-op returning 0. Real impls: R5/R6 (desktop).\n\n")
    for s in data:
        f.write("unsigned char {}[{}];\n".format(s, "0x{:X}".format(table_sizes[s]) if s in table_sizes else 1))
    f.write("\n")
    for s in funcs:
        f.write("long {}() {{ return 0; }}\n".format(s))

print("link stubs: {} data, {} funcs -> {}".format(len(data), len(funcs), OUT))
