#!/usr/bin/env python3
"""Generate f3d.o2r — the Fast3D shader archive — from OUR Kenix3 libultraship shader sources.

f3d.o2r is just a zip of the shader source files under "shaders/<backend>/...". libultraship's
Fast3D backends load them by that path during window init. We build it from our own engine's
shaders (NOT a borrowed one from another libultraship fork, which may be incompatible).
"""
import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "libultraship", "src", "fast", "shaders")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "x64", "port", "f3d.o2r")

with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(SRC):
        for f in files:
            full = os.path.join(root, f)
            arc = "shaders/" + os.path.relpath(full, SRC).replace(os.sep, "/")
            z.write(full, arc)

print("wrote", OUT)
for n in zipfile.ZipFile(OUT).namelist():
    print(" ", n)
