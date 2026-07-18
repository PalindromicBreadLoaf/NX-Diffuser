#!/usr/bin/env python3
"""Generate f3d.o2r from our Fast3D shaders and port-owned GUI texture assets.

Fast3D loads shaders from ``shaders/<backend>/...`` during window init. Its GUI texture loader uses
the same resource manager, so assets under ``port/assets/textures`` are archived as ``textures/...``.
The shaders always come from this checkout's libultraship rather than another fork.

The archive is written deterministically (sorted entry order, fixed timestamps) so identical
inputs produce byte-identical output on any machine — required for content hashing, build
caching, and meaningful copy_if_different behavior.
"""
import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "libultraship", "src", "fast", "shaders")
GUI_TEXTURES = os.path.join(REPO, "port", "assets", "textures")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "x64", "port", "f3d.o2r")

# Fixed timestamp for all entries: zip format's epoch (1980-01-01).
FIXED_DATE = (1980, 1, 1, 0, 0, 0)


def collect(base, prefix, predicate=lambda name: True):
    entries = []
    for root, dirs, files in os.walk(base):
        dirs.sort()
        for f in sorted(files):
            if not predicate(f):
                continue
            full = os.path.join(root, f)
            arc = prefix + os.path.relpath(full, base).replace(os.sep, "/")
            entries.append((arc, full))
    return sorted(entries)


def write_deterministic(z, arc, full):
    with open(full, "rb") as fh:
        data = fh.read()
    info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


entries = collect(SRC, "shaders/") + collect(GUI_TEXTURES, "textures/",
                                             lambda name: name.lower().endswith(".png"))

with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for arc, full in entries:
        write_deterministic(z, arc, full)

print("wrote", OUT)
for n in zipfile.ZipFile(OUT).namelist():
    print(" ", n)
