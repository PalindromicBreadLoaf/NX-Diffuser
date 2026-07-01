#!/usr/bin/env python3
"""G-Diffuser asset binding generator (Slice 4c / R2).

Emits port/gen/AssetBindings.c, which DEFINES every decomp asset symbol as a (placeholder)
array of its declared C type. This keeps the symbols as arrays — matching Torch's
`extern <ctype> <sym>[];` headers — so:
  * the symbols are defined (no undefined-symbol link errors), and
  * their addresses are compile-time constants, so the game's STATIC tables / display lists
    that reference assets still compile (a runtime pointer can't be a static initializer).

Torch's generated headers (with their _WIDTH/_HEIGHT/_COMPRESSED_SIZE #defines) are used as-is;
no shadow headers are produced.

NOTE: arrays are placeholders ([1]). Filling them with real asset data from the .o2r at the
right sizes is R6 work (needs runtime on a real display). GDiffuser_LoadAllAssets() is the hook.

R2b addition: also emits gdx_lookup_common_asset_rom_offset() — a lookup table that maps each
common_assets_compressed stub address to its absolute ROM byte offset. Used by the PORT path of
func_80077CF0 (object.c) so MIO0 decompression reads from the right place in gdx_rom_buffer.
"""
import glob
import os
import re
import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_YAML_DIR = os.path.join(REPO, "decomp", "assets", "yaml", "us", "rev0")
BINDING_C = os.path.join(REPO, "port", "gen", "AssetBindings.c")

os.makedirs(os.path.dirname(BINDING_C), exist_ok=True)

defs = []
total = 0

# Track common_assets_compressed entries for the ROM offset table and O2R key table.
# Each entry: (symbol_name, absolute_rom_offset, o2r_key)
common_asset_rom_entries = []

# Track real ROM-backed assets for the display-list bridge. The decomp build
# keeps asset symbols as one-byte placeholders, so runtime GBI commands contain
# truncated placeholder addresses. This map lets the port resolve those addresses
# back into loaded ROM segment images.
asset_segment_entries = []
asset_range_entries = []
asset_fixup_entries = []
segment_images = {}
asset_load_entries = []

FIXUP_NONE = 0
FIXUP_GFX = 1
FIXUP_VP = 2
FIXUP_VTX = 3

TEXTURE_BYTES_PER_PIXEL = {
    "RGBA16": 2,
    "RGBA32": 4,
    "IA4": 0.5,
    "IA8": 1,
    "IA16": 2,
    "I4": 0.5,
    "I8": 1,
    "CI4": 0.5,
    "CI8": 1,
}


def asset_declared_size(val):
    typ = val.get("type")
    if typ == "VP":
        return 16
    if typ == "VTX":
        return int(val.get("count", 1)) * 16
    if typ == "BLOB":
        return int(val.get("size", 0))
    if typ == "ARRAY":
        count = int(val.get("count", 0))
        elem = val.get("array_type", val.get("ctype", "u8"))
        elem_size = {
            "s8": 1, "u8": 1,
            "s16": 2, "u16": 2,
            "s32": 4, "u32": 4,
            "s64": 8, "u64": 8,
        }.get(elem, 1)
        return count * elem_size
    if typ in ("TEXTURE", "COMPRESSED_TEXTURE"):
        if "size" in val:
            return int(val["size"])
        width = val.get("width")
        height = val.get("height")
        fmt = val.get("format")
        if width is not None and height is not None and fmt in TEXTURE_BYTES_PER_PIXEL:
            return int(int(width) * int(height) * TEXTURE_BYTES_PER_PIXEL[fmt])
    return 0


def asset_definition_type_and_count(val):
    typ = val.get("type")
    if typ == "ARRAY":
        return val.get("array_type", val.get("ctype", "u8")), max(1, int(val.get("count", 1)))
    return val.get("ctype", "u8"), 1


def fixup_kind(val):
    typ = val.get("type")
    if typ == "GFX":
        return FIXUP_GFX
    if typ == "VP":
        return FIXUP_VP
    if typ == "VTX":
        return FIXUP_VTX
    return FIXUP_NONE

for path in sorted(glob.glob(os.path.join(ASSET_YAML_DIR, "*.yaml"))):
    fname = os.path.basename(path)
    yaml_stem = os.path.splitext(fname)[0]
    is_common = (fname == "common_assets_compressed.yaml")

    with open(path) as f:
        yaml_text = f.read()
    data = yaml.safe_load(yaml_text) or {}

    # Some segment YAMLs record the decoded image size in a trailing comment.
    # Keep it so the final variable-length GFX entry has an upper boundary even
    # though there is no following asset offset from which to infer its size.
    size_matches = re.findall(r"(?m)^\s*#\s*size\s*=\s*(0x[0-9A-Fa-f]+|\d+)", yaml_text)
    segment_declared_size = int(size_matches[-1], 0) if size_matches else 0

    # Extract ROM base offset and segment id from :config: segments.
    rom_base = None
    segment_id = None
    config = data.get(":config", {}) or {}
    segs = config.get("segments") or []
    if segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2:
        segment_id = segs[0][0]
        rom_base = segs[0][1]  # e.g. 0x2B9EA0

    compressed = bool((config.get("compression") or {}).get("offset") is not None)

    # YAML table declarations describe contiguous symbol ranges whose base
    # names are used directly by game code for pointer arithmetic. They are
    # not regular asset entries, so preserve their placeholder symbol as a
    # token and teach the runtime resolver how offsets from that token map
    # into the real segment image.
    tables = config.get("tables") or {}
    if (not is_common) and segment_id is not None and rom_base is not None:
        for table_name, table_val in tables.items():
            if not isinstance(table_val, dict):
                continue
            table_range = table_val.get("range")
            if not isinstance(table_range, (list, tuple)) or len(table_range) < 2:
                continue
            range_start = int(table_range[0])
            range_end = int(table_range[1])
            if range_end <= range_start:
                continue
            range_segment = (range_start >> 24) & 0xFF
            if range_segment != int(segment_id):
                continue
            asset_range_entries.append((
                table_name,
                int(segment_id),
                int(rom_base),
                int(compressed),
                range_start & 0x00FFFFFF,
                range_end - range_start,
                yaml_stem,
            ))

    # GFX entries are variable-length display lists. The next YAML offset is
    # the best source of truth for how many command bytes to endian-fix.
    asset_items = []
    for item_key, item_val in data.items():
        if isinstance(item_val, dict) and not str(item_key).startswith(":") and item_val.get("offset") is not None:
            asset_items.append((int(item_val.get("offset")), item_key, item_val))
    asset_items.sort(key=lambda item: item[0])
    next_offsets = {}
    for idx, (offset, item_key, _item_val) in enumerate(asset_items):
        if idx + 1 < len(asset_items):
            next_offsets[item_key] = asset_items[idx + 1][0]

    for key, val in data.items():
        if not isinstance(val, dict) or str(key).startswith(":"):
            continue
        sym = val.get("symbol", key)
        ctype, count = asset_definition_type_and_count(val)
        defs.append("{} {}[{}];".format(ctype, sym, count))
        total += 1

        if is_common and rom_base is not None:
            offset = val.get("offset")
            if offset is not None:
                o2r_key = "common_assets_compressed/{}".format(sym)
                common_asset_rom_entries.append((sym, rom_base + offset, o2r_key))
                if val.get("type") == "ARRAY":
                    asset_load_entries.append((sym, o2r_key))

        # common_assets_compressed is a packed set of individually-compressed
        # payloads used by object.c; it is not a normal segment image.
        if (not is_common) and (segment_id is not None) and (rom_base is not None):
            offset = val.get("offset")
            if offset is not None:
                offset = int(offset)
                declared = asset_declared_size(val)
                if val.get("type") == "GFX":
                    gfx_end = next_offsets.get(key)
                    if gfx_end is None and segment_declared_size > offset:
                        gfx_end = segment_declared_size
                    declared = max(0, int(gfx_end if gfx_end is not None else offset) - offset)

                image_key = (int(segment_id), int(rom_base), int(compressed))
                image_size = segment_images.get(image_key, 0)
                if segment_declared_size > 0:
                    image_size = max(image_size, segment_declared_size)
                if declared > 0:
                    image_size = max(image_size, offset + declared)
                elif key in next_offsets:
                    image_size = max(image_size, int(next_offsets[key]))
                else:
                    image_size = max(image_size, offset)
                segment_images[image_key] = image_size

                asset_segment_entries.append((sym, int(segment_id), int(rom_base), int(compressed), offset, image_key, yaml_stem))

                kind = fixup_kind(val)
                if kind != FIXUP_NONE and declared > 0:
                    asset_fixup_entries.append((int(segment_id), int(rom_base), offset, declared, kind))

# Build the ROM offset lookup table (PORT only).
lookup_lines = []
lookup_lines.append("")
lookup_lines.append("#ifdef PORT")
lookup_lines.append("/* PORT: maps each common_assets_compressed stub address to its")
lookup_lines.append(" * absolute ROM byte offset so func_80077CF0 can read from gdx_rom_buffer. */")
lookup_lines.append("typedef struct { void* sym; unsigned int rom_offset; const char* o2r_key; } GdxCommonAssetEntry;")
lookup_lines.append("static const GdxCommonAssetEntry sCommonAssetRomMap[] = {")
for sym, rom_offset, o2r_key in common_asset_rom_entries:
    lookup_lines.append("    {{ {}, 0x{:08X}U, \"{}\" }},".format(sym, rom_offset, o2r_key))
lookup_lines.append("    { NULL, 0U, NULL }")
lookup_lines.append("};")
lookup_lines.append("")
lookup_lines.append("unsigned int gdx_lookup_common_asset_rom_offset(unsigned long long sym_addr) {")
lookup_lines.append("    int i;")
lookup_lines.append("    for (i = 0; sCommonAssetRomMap[i].sym != NULL; i++) {")
lookup_lines.append("        if ((unsigned long long)sCommonAssetRomMap[i].sym == sym_addr)")
lookup_lines.append("            return sCommonAssetRomMap[i].rom_offset;")
lookup_lines.append("    }")
lookup_lines.append("    return 0U;")
lookup_lines.append("}")
lookup_lines.append("")
lookup_lines.append("const char* gdx_lookup_common_asset_o2r_key(unsigned long long sym_addr) {")
lookup_lines.append("    int i;")
lookup_lines.append("    for (i = 0; sCommonAssetRomMap[i].sym != NULL; i++) {")
lookup_lines.append("        if ((unsigned long long)sCommonAssetRomMap[i].sym == sym_addr)")
lookup_lines.append("            return sCommonAssetRomMap[i].o2r_key;")
lookup_lines.append("    }")
lookup_lines.append("    return NULL;")
lookup_lines.append("}")
lookup_lines.append("#endif /* PORT */")

asset_lines = []
asset_lines.append("")
asset_lines.append("#ifdef PORT")
asset_lines.append("/* PORT: maps generated one-byte asset placeholder symbols to their")
asset_lines.append(" * real ROM-backed segment image and offset for the GBI bridge. */")
asset_lines.append("typedef struct { void* sym; unsigned char segment; unsigned int rom_base; unsigned char compressed; unsigned int offset; unsigned int image_size; unsigned int sym_size; const char* o2r_key; } GdxAssetSegmentEntry;")
asset_lines.append("static const GdxAssetSegmentEntry sAssetSegmentMap[] = {")
for sym, segment, rom_base, compressed, offset, image_key, yaml_stem in asset_segment_entries:
    image_size = segment_images.get(image_key, 0)
    if image_size <= 0:
        continue
    o2r_key = "{}/{}".format(yaml_stem, sym)
    asset_lines.append("    {{ {}, 0x{:02X}u, 0x{:08X}U, {}u, 0x{:08X}U, 0x{:08X}U, (unsigned int)sizeof({}), \"{}\" }},".format(
        sym, segment, rom_base, compressed, offset, image_size, sym, o2r_key))
asset_lines.append("    { NULL, 0u, 0U, 0u, 0U, 0U, 0U, NULL }")
asset_lines.append("};")
asset_lines.append("")
asset_lines.append("typedef struct { void* sym; unsigned char segment; unsigned int rom_base; unsigned char compressed; unsigned int offset; unsigned int size; unsigned int image_size; const char* o2r_key; } GdxAssetRangeEntry;")
asset_lines.append("static const GdxAssetRangeEntry sAssetRangeMap[] = {")
for sym, segment, rom_base, compressed, offset, size, yaml_stem in asset_range_entries:
    image_key = (segment, rom_base, compressed)
    image_size = segment_images.get(image_key, 0)
    if image_size <= 0:
        continue
    o2r_key = "{}/{}".format(yaml_stem, sym)
    asset_lines.append("    {{ {}, 0x{:02X}u, 0x{:08X}U, {}u, 0x{:08X}U, 0x{:08X}U, 0x{:08X}U, \"{}\" }},".format(
        sym, segment, rom_base, compressed, offset, size, image_size, o2r_key))
asset_lines.append("    { NULL, 0u, 0U, 0u, 0U, 0U, 0U, NULL }")
asset_lines.append("};")
asset_lines.append("")
asset_lines.append("/* Symbol lookups run for every translated pointer of every display-list")
asset_lines.append(" * command each frame. A linear scan of the segment map dominates frame")
asset_lines.append(" * time, so both lookups binary-search a lazily built sorted index. */")
asset_lines.append("typedef struct { unsigned int low32; int idx; } GdxAssetIndexEntry;")
asset_lines.append("static GdxAssetIndexEntry sAssetSegmentIndex[sizeof(sAssetSegmentMap) / sizeof(sAssetSegmentMap[0])];")
asset_lines.append("static int sAssetSegmentIndexCount = 0;")
asset_lines.append("static int sAssetSegmentIndexBuilt = 0;")
asset_lines.append("")
asset_lines.append("static void gdx_build_asset_index(void) {")
asset_lines.append("    int n, gap, i, j;")
asset_lines.append("    if (sAssetSegmentIndexBuilt) return;")
asset_lines.append("    for (n = 0; sAssetSegmentMap[n].sym != NULL; n++) {")
asset_lines.append("        sAssetSegmentIndex[n].low32 = (unsigned int)(unsigned long long)sAssetSegmentMap[n].sym;")
asset_lines.append("        sAssetSegmentIndex[n].idx = n;")
asset_lines.append("    }")
asset_lines.append("    for (gap = n / 2; gap > 0; gap /= 2) {")
asset_lines.append("        for (i = gap; i < n; i++) {")
asset_lines.append("            GdxAssetIndexEntry t = sAssetSegmentIndex[i];")
asset_lines.append("            for (j = i; j >= gap && sAssetSegmentIndex[j - gap].low32 > t.low32; j -= gap) {")
asset_lines.append("                sAssetSegmentIndex[j] = sAssetSegmentIndex[j - gap];")
asset_lines.append("            }")
asset_lines.append("            sAssetSegmentIndex[j] = t;")
asset_lines.append("        }")
asset_lines.append("    }")
asset_lines.append("    sAssetSegmentIndexCount = n;")
asset_lines.append("    sAssetSegmentIndexBuilt = 1;")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("/* Greatest index entry with low32 <= key, or -1. */")
asset_lines.append("static int gdx_asset_index_floor(unsigned int key) {")
asset_lines.append("    int lo = 0, hi, best = -1;")
asset_lines.append("    gdx_build_asset_index();")
asset_lines.append("    hi = sAssetSegmentIndexCount - 1;")
asset_lines.append("    while (lo <= hi) {")
asset_lines.append("        int mid = lo + (hi - lo) / 2;")
asset_lines.append("        if (sAssetSegmentIndex[mid].low32 <= key) { best = mid; lo = mid + 1; }")
asset_lines.append("        else { hi = mid - 1; }")
asset_lines.append("    }")
asset_lines.append("    return best;")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("int gdx_lookup_asset_segment(unsigned int sym_low32, unsigned char* segment, unsigned int* rom_base,")
asset_lines.append("                             unsigned char* compressed, unsigned int* offset, unsigned int* image_size) {")
asset_lines.append("    int i;")
asset_lines.append("    int f = gdx_asset_index_floor(sym_low32);")
asset_lines.append("    if (f >= 0 && sAssetSegmentIndex[f].low32 == sym_low32) {")
asset_lines.append("        i = sAssetSegmentIndex[f].idx;")
asset_lines.append("        if (segment != NULL) *segment = sAssetSegmentMap[i].segment;")
asset_lines.append("        if (rom_base != NULL) *rom_base = sAssetSegmentMap[i].rom_base;")
asset_lines.append("        if (compressed != NULL) *compressed = sAssetSegmentMap[i].compressed;")
asset_lines.append("        if (offset != NULL) *offset = sAssetSegmentMap[i].offset;")
asset_lines.append("        if (image_size != NULL) *image_size = sAssetSegmentMap[i].image_size;")
asset_lines.append("        return 1;")
asset_lines.append("    }")
asset_lines.append("    for (i = 0; sAssetRangeMap[i].sym != NULL; i++) {")
asset_lines.append("        unsigned int base = (unsigned int)(unsigned long long)sAssetRangeMap[i].sym;")
asset_lines.append("        unsigned int delta = sym_low32 - base;")
asset_lines.append("        if (delta < sAssetRangeMap[i].size) {")
asset_lines.append("            if (segment != NULL) *segment = sAssetRangeMap[i].segment;")
asset_lines.append("            if (rom_base != NULL) *rom_base = sAssetRangeMap[i].rom_base;")
asset_lines.append("            if (compressed != NULL) *compressed = sAssetRangeMap[i].compressed;")
asset_lines.append("            if (offset != NULL) *offset = sAssetRangeMap[i].offset + delta;")
asset_lines.append("            if (image_size != NULL) *image_size = sAssetRangeMap[i].image_size;")
asset_lines.append("            return 1;")
asset_lines.append("        }")
asset_lines.append("    }")
asset_lines.append("    return 0;")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("/* Interior-pointer resolution: game DLs reference vertex data at symbol+offset")
asset_lines.append(" * (e.g. gSPVertex(&D_3000C98[64], ...)). Exact matching misses those, and the")
asset_lines.append(" * pointer would otherwise resolve into the zero-filled placeholder BSS array,")
asset_lines.append(" * producing origin-vertex spike polygons. Match within each symbol's byte size. */")
asset_lines.append("int gdx_lookup_asset_segment_interior(unsigned int sym_low32, unsigned char* segment, unsigned int* rom_base,")
asset_lines.append("                                      unsigned char* compressed, unsigned int* offset, unsigned int* image_size) {")
asset_lines.append("    int i;")
asset_lines.append("    unsigned int base, delta;")
asset_lines.append("    int f = gdx_asset_index_floor(sym_low32);")
asset_lines.append("    if (f < 0) return 0;")
asset_lines.append("    /* Placeholder arrays are distinct linker objects, so address ranges never")
asset_lines.append("       overlap: only the greatest base at or below the pointer can contain it. */")
asset_lines.append("    i = sAssetSegmentIndex[f].idx;")
asset_lines.append("    base = sAssetSegmentIndex[f].low32;")
asset_lines.append("    delta = sym_low32 - base;")
asset_lines.append("    if (delta != 0u && delta < sAssetSegmentMap[i].sym_size) {")
asset_lines.append("        if (segment != NULL) *segment = sAssetSegmentMap[i].segment;")
asset_lines.append("        if (rom_base != NULL) *rom_base = sAssetSegmentMap[i].rom_base;")
asset_lines.append("        if (compressed != NULL) *compressed = sAssetSegmentMap[i].compressed;")
asset_lines.append("        if (offset != NULL) *offset = sAssetSegmentMap[i].offset + delta;")
asset_lines.append("        if (image_size != NULL) *image_size = sAssetSegmentMap[i].image_size;")
asset_lines.append("        return 1;")
asset_lines.append("    }")
asset_lines.append("    return 0;")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("const char* gdx_lookup_asset_segment_o2r_key(unsigned int sym_low32) {")
asset_lines.append("    int i;")
asset_lines.append("    for (i = 0; sAssetSegmentMap[i].sym != NULL; i++) {")
asset_lines.append("        if ((unsigned int)(unsigned long long)sAssetSegmentMap[i].sym == sym_low32)")
asset_lines.append("            return sAssetSegmentMap[i].o2r_key;")
asset_lines.append("    }")
asset_lines.append("    return NULL;")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("const char* gdx_find_o2r_key_by_abs_rom_offset(unsigned int abs_rom_offset) {")
asset_lines.append("    int i;")
asset_lines.append("    for (i = 0; sAssetSegmentMap[i].sym != NULL; i++) {")
asset_lines.append("        if (sAssetSegmentMap[i].compressed == 0u &&")
asset_lines.append("            sAssetSegmentMap[i].rom_base + sAssetSegmentMap[i].offset == abs_rom_offset)")
asset_lines.append("            return sAssetSegmentMap[i].o2r_key;")
asset_lines.append("    }")
asset_lines.append("    return NULL;")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("typedef struct { unsigned char segment; unsigned int rom_base; unsigned int offset; unsigned int size; unsigned char kind; } GdxAssetFixupEntry;")
asset_lines.append("static const GdxAssetFixupEntry sAssetFixups[] = {")
for segment, rom_base, offset, size, kind in asset_fixup_entries:
    asset_lines.append("    {{ 0x{:02X}u, 0x{:08X}U, 0x{:08X}U, 0x{:08X}U, {}u }},".format(
        segment, rom_base, offset, size, kind))
asset_lines.append("    { 0u, 0U, 0U, 0U, 0u }")
asset_lines.append("};")
asset_lines.append("")
asset_lines.append("static unsigned short gdx_bswap16(unsigned short v) {")
asset_lines.append("    return (unsigned short)((v << 8) | (v >> 8));")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("static unsigned int gdx_bswap32(unsigned int v) {")
asset_lines.append("    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) |")
asset_lines.append("           ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24);")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("void gdx_fixup_asset_segment_image(unsigned char segment, unsigned int rom_base, unsigned char* data, unsigned int size) {")
asset_lines.append("    int i;")
asset_lines.append("    for (i = 0; sAssetFixups[i].kind != 0u; i++) {")
asset_lines.append("        unsigned int j;")
asset_lines.append("        unsigned int off = sAssetFixups[i].offset;")
asset_lines.append("        unsigned int bytes = sAssetFixups[i].size;")
asset_lines.append("        if (sAssetFixups[i].segment != segment || sAssetFixups[i].rom_base != rom_base) continue;")
asset_lines.append("        if (off >= size || bytes > size - off) continue;")
asset_lines.append("        if (sAssetFixups[i].kind == 1u) {")
asset_lines.append("            for (j = 0; j + 4 <= bytes; j += 4) {")
asset_lines.append("                unsigned int* p = (unsigned int*)(void*)(data + off + j);")
asset_lines.append("                *p = gdx_bswap32(*p);")
asset_lines.append("            }")
asset_lines.append("        } else if (sAssetFixups[i].kind == 2u) {")
asset_lines.append("            for (j = 0; j + 2 <= bytes; j += 2) {")
asset_lines.append("                unsigned short* p = (unsigned short*)(void*)(data + off + j);")
asset_lines.append("                *p = gdx_bswap16(*p);")
asset_lines.append("            }")
asset_lines.append("        } else if (sAssetFixups[i].kind == 3u) {")
asset_lines.append("            for (j = 0; j + 16 <= bytes; j += 16) {")
asset_lines.append("                unsigned int k;")
asset_lines.append("                for (k = 0; k < 12; k += 2) {")
asset_lines.append("                    unsigned short* p = (unsigned short*)(void*)(data + off + j + k);")
asset_lines.append("                    *p = gdx_bswap16(*p);")
asset_lines.append("                }")
asset_lines.append("            }")
asset_lines.append("        }")
asset_lines.append("    }")
asset_lines.append("}")
asset_lines.append("")
asset_lines.append("void gdx_register_asset_segment_command_ranges(unsigned char segment, unsigned int rom_base, unsigned char* data, unsigned int size) {")
asset_lines.append("    int i;")
asset_lines.append("    extern void gdx_register_host_n64_command_range(void* ptr, size_t size);")
asset_lines.append("    for (i = 0; sAssetFixups[i].kind != 0u; i++) {")
asset_lines.append("        unsigned int off = sAssetFixups[i].offset;")
asset_lines.append("        unsigned int bytes = sAssetFixups[i].size;")
asset_lines.append("        if (sAssetFixups[i].kind != 1u) continue;")
asset_lines.append("        if (sAssetFixups[i].segment != segment || sAssetFixups[i].rom_base != rom_base) continue;")
asset_lines.append("        if (off >= size || bytes > size - off) continue;")
asset_lines.append("        gdx_register_host_n64_command_range(data + off, bytes);")
asset_lines.append("    }")
asset_lines.append("}")
asset_lines.append("#endif /* PORT */")

with open(BINDING_C, "w") as f:
    f.write("// AUTO-GENERATED by tools/gen_asset_bindings.py (R2). Do not edit by hand.\n")
    f.write("// Placeholder array definitions for every decomp asset symbol (kept as arrays so\n")
    f.write("// static asset references compile). Real size + .o2r data load = R6.\n")
    f.write('#include "global.h"\n')
    f.write('\n')
    # YAML table ranges are addressable placeholder tokens but are not regular
    # asset entries, so their declarations are not present in Torch headers.
    # Emit them here before sAssetRangeMap takes their addresses.
    for sym in sorted({entry[0] for entry in asset_range_entries}):
        f.write("extern unsigned char {}[];\n".format(sym))
    if asset_range_entries:
        f.write('\n')
    f.write("\n".join(defs))
    f.write("\n\nvoid GDiffuser_LoadAllAssets(void) {\n")
    f.write("#ifdef PORT\n")
    f.write("    size_t copiedSize;\n")
    f.write("    extern int GDiffuser_LoadAssetBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);\n")
    for sym, o2r_key in asset_load_entries:
        f.write("    copiedSize = 0;\n")
        f.write("    (void)GDiffuser_LoadAssetBytes(\"{}\", {}, sizeof({}), &copiedSize);\n".format(o2r_key, sym, sym))
    f.write("#endif\n")
    f.write("}\n")
    f.write("\n".join(lookup_lines))
    f.write("\n".join(asset_lines))
    f.write("\n")

print("R2: defined {} asset symbols -> {}".format(total, BINDING_C))
print("R2b: {} common_assets_compressed ROM offset entries".format(len(common_asset_rom_entries)))
print("R6: {} ROM-backed asset segment entries, {} fixups".format(len(asset_segment_entries), len(asset_fixup_entries)))
