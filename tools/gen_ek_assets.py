#!/usr/bin/env python3
"""G-Diffuser Expansion Kit asset generator (EK slice 1).

Reads the disk-side asset yamls from the fzerox-expansion-kit decomp and emits:
  * include/assets/us/ek/**.h        -- Torch-style headers (extern decls + WIDTH/HEIGHT
                                        defines) so EK sources compile with ASSET_VERSION=us.
  * port/gen/EkAssetBindings.c       -- real-size array DEFINITIONS for every EK asset
                                        symbol (never 1-byte stubs: indexed tables and the
                                        runtime resolvers need real storage sizes), plus
                                        gdx_ek_assets_fill() copying data from the 64DD
                                        disk image for every yaml that declares a disk
                                        segment base. Entries without a segment base are
                                        defined zero-filled (delivered later via MFS).

The disk image is treated as linear: yaml offsets are relative to the yaml's
segment base byte offset within the .ndd dump.
"""
import glob
import os

import yaml

import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EK_YAML_DIR = os.path.join(REPO, "fzerox-expansion-kit", "assets", "yaml", "jp")
HEADER_OUT_DIR = os.path.join(REPO, "include", "assets", "us", "ek")
PAYLOAD_OUT_DIR = os.path.join(REPO, "decomp", "src", "assets", "us", "ek")
BINDING_C = os.path.join(REPO, "port", "gen", "EkAssetBindings.c")
DECOMP_SRC = os.path.join(REPO, "decomp", "src")


def scan_payload_includes():
    """Payload .c paths the decomp includes via ASSET_SOURCE_EK(...).

    Torch emits asset payload C files (initialized arrays); several EK data
    files #include them directly, so the yaml's symbols must be DEFINED in a
    generated payload file rather than in EkAssetBindings.c (the include would
    otherwise produce duplicate definitions at link)."""
    paths = set()
    for root, _dirs, files in os.walk(DECOMP_SRC):
        if os.sep + "assets" in root:
            continue
        for name in files:
            if not name.endswith((".c", ".h")):
                continue
            with open(os.path.join(root, name), encoding="utf-8", errors="ignore") as f:
                for m in re.finditer(r"ASSET_SOURCE_EK\(([^)]+\.c)\)", f.read()):
                    paths.add(m.group(1).strip())
    return paths

TEXTURE_BPP = {
    "RGBA16": 2, "RGBA32": 4, "IA16": 2, "IA8": 1, "IA4": 0.5,
    "I8": 1, "I4": 0.5, "CI8": 1, "CI4": 0.5, "TLUT": 2,
}

CTYPE_FOR_TYPE = {"GFX": "Gfx", "VTX": "Vtx", "MTX": "Mtx"}


def entry_ctype(val):
    if val.get("ctype"):
        return val["ctype"]
    return CTYPE_FOR_TYPE.get(val.get("type"), "u8")


def entry_size(val, next_offset):
    # COMPRESSED_TEXTURE symbols hold the compressed stream (the game inflates
    # at runtime), so their storage size is the on-disk span, not w*h*bpp.
    if val.get("type") == "TEXTURE":
        bpp = TEXTURE_BPP.get(val.get("format", ""), 2)
        w = int(val.get("width", 0))
        h = int(val.get("height", 0))
        size = int(w * h * bpp)
        if size > 0:
            return size
    offset = val.get("offset")
    if offset is not None and next_offset is not None and next_offset > int(offset):
        return next_offset - int(offset)
    return 8


def ctype_bytes(ctype):
    return {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4,
            "f32": 4, "Gfx": 8, "Vtx": 16, "Mtx": 64}.get(ctype, 1)


headers = []       # (relative_header_path, [lines])
definitions = []   # (ctype, sym, element_count, byte_size)
payloads = []      # (relative_payload_c_path, [definition lines])
fills = []         # (sym, disk_byte_offset, byte_size)
seen_syms = set()
payload_includes = scan_payload_includes()

for path in sorted(glob.glob(os.path.join(EK_YAML_DIR, "**", "*.yaml"), recursive=True)):
    rel = os.path.relpath(path, EK_YAML_DIR).replace("\\", "/")
    stem_rel = os.path.splitext(rel)[0]
    stem = os.path.basename(stem_rel)
    stem_dir = os.path.dirname(stem_rel)
    # Torch payload path convention: <yaml dir>/<stem>/<stem>.c
    payload_rel = ("{}/{}/{}.c".format(stem_dir, stem, stem) if stem_dir
                   else "{}/{}.c".format(stem, stem))
    is_payload = payload_rel in payload_includes
    payload_lines = []

    with open(path) as f:
        data = yaml.safe_load(f.read()) or {}

    config = data.get(":config", {}) or {}
    segs = config.get("segments") or []
    disk_base = None
    if segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2:
        disk_base = int(segs[0][1])

    items = []
    for key, val in data.items():
        if isinstance(val, dict) and not str(key).startswith(":") and val.get("offset") is not None:
            items.append((int(val["offset"]), key, val))
    items.sort(key=lambda item: item[0])
    next_offsets = {}
    for idx, (offset, key, _val) in enumerate(items):
        if idx + 1 < len(items):
            next_offsets[key] = items[idx + 1][0]

    guard = stem_rel.replace("/", "_").replace("-", "_").upper() + "_H"
    lines = ["#ifndef {}".format(guard), "#define {}".format(guard), "", '#include "gfx.h"', ""]

    for key, val in data.items():
        if not isinstance(val, dict) or str(key).startswith(":"):
            continue
        sym = val.get("symbol", key)
        ctype = entry_ctype(val)
        size = entry_size(val, next_offsets.get(key))
        lines.append("extern {} {}[];".format(ctype, sym))
        if val.get("type") in ("TEXTURE", "COMPRESSED_TEXTURE"):
            lines.append("#define _{}_WIDTH 0x{:x}".format(sym, int(val.get("width", 0))))
            lines.append("#define _{}_HEIGHT 0x{:x}".format(sym, int(val.get("height", 0))))
        if val.get("type") == "COMPRESSED_TEXTURE" or val.get("compression"):
            lines.append("#define _{}_COMPRESSED_SIZE 0x{:x}".format(sym, size))

        if sym in seen_syms:
            continue
        seen_syms.add(sym)

        elem = ctype_bytes(ctype)
        count = max(1, (size + elem - 1) // elem)
        if is_payload:
            payload_lines.append("{} {}[{}];".format(ctype, sym, count))
        else:
            definitions.append((ctype, sym, count, size))
        if disk_base is not None and val.get("offset") is not None:
            fills.append((sym, disk_base + int(val["offset"]), size))

    lines += ["", "#endif", ""]
    headers.append((stem_rel + ".h", lines))
    if is_payload:
        payloads.append((payload_rel, payload_lines))

for rel_header, lines in headers:
    out_path = os.path.join(HEADER_OUT_DIR, rel_header)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        f.write("\n".join(["// AUTO-GENERATED by tools/gen_ek_assets.py. Do not edit by hand."] + lines))

for rel_payload, lines in payloads:
    out_path = os.path.join(PAYLOAD_OUT_DIR, rel_payload)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    # Torch payloads carry the asset dimension #defines with them; consumers
    # (e.g. course_select.c struct initializers) rely on that, so include the
    # generated header alongside the zero-filled definitions.
    header_rel = os.path.dirname(os.path.dirname(rel_payload))
    stem = os.path.splitext(os.path.basename(rel_payload))[0]
    header_path = "{}/{}.h".format(header_rel, stem) if header_rel else "{}.h".format(stem)
    with open(out_path, "w") as f:
        f.write("// AUTO-GENERATED by tools/gen_ek_assets.py. Zero-filled asset payload\n")
        f.write("// definitions (data arrives at runtime via gdx_ek_assets_fill).\n")
        f.write('#include "assets/us/ek/{}"\n'.format(header_path))
        f.write("\n".join(lines))
        f.write("\n")

with open(BINDING_C, "w") as f:
    f.write("// AUTO-GENERATED by tools/gen_ek_assets.py. Do not edit by hand.\n")
    f.write("// Real-size definitions for Expansion Kit disk asset symbols plus the\n")
    f.write("// disk-image fill table. Compiled only when GDX_EXPANSION_KIT is enabled.\n")
    f.write('#include "global.h"\n\n')
    for ctype, sym, count, _size in definitions:
        f.write("{} {}[{}];\n".format(ctype, sym, count))
    f.write("\ntypedef struct { void* dest; unsigned int diskOffset; unsigned int size; } GdxEkAssetFill;\n")
    f.write("static const GdxEkAssetFill sEkAssetFills[] = {\n")
    for sym, disk_off, size in fills:
        f.write("    {{ {}, 0x{:08X}U, 0x{:X}U }},\n".format(sym, disk_off, size))
    f.write("    { 0, 0U, 0U }\n};\n\n")
    f.write("/* Copies EK asset payloads out of the loaded 64DD disk image. Big-endian\n")
    f.write(" * u16 texel data is byte-swapped per 16-bit word elsewhere by the gfx\n")
    f.write(" * bridge's raw N64 range handling, so this stays a straight copy. */\n")
    f.write("void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize) {\n")
    f.write("    int i;\n")
    f.write("    unsigned int b;\n")
    f.write("    if (disk == 0) { return; }\n")
    f.write("    for (i = 0; sEkAssetFills[i].dest != 0; i++) {\n")
    f.write("        if ((unsigned long long)sEkAssetFills[i].diskOffset + sEkAssetFills[i].size <= diskSize) {\n")
    f.write("            unsigned char* dest = (unsigned char*)sEkAssetFills[i].dest;\n")
    f.write("            const unsigned char* src = disk + sEkAssetFills[i].diskOffset;\n")
    f.write("            /* byte loop: this TU compiles with the decomp's headers, which\n")
    f.write("               clash with the MSVC CRT's <string.h> */\n")
    f.write("            for (b = 0; b < sEkAssetFills[i].size; b++) {\n")
    f.write("                dest[b] = src[b];\n")
    f.write("            }\n")
    f.write("        }\n")
    f.write("    }\n")
    f.write("}\n")

print("EK assets: {} headers, {} symbol defs, {} disk fills".format(len(headers), len(definitions), len(fills)))
