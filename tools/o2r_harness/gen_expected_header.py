#!/usr/bin/env python3
"""Emit port/gen/gdx_o2r_expected.h from a validated deterministic archive.

Produces the single code-level contract (C3) between the harness (1-C) and
first-boot validation (1-B):

  #define GDX_O2R_EXPECTED_SHA256      "<hex>"
  #define GDX_O2R_EXPECTED_ENTRY_COUNT <n>

IMPORTANT: the input MUST be the archive produced by the deterministic
gdx-extract, NOT the legacy build-time archive (which predates determinism and
has an unstable SHA-256). The orchestrator runs this only after
verify_determinism.py + validate_archive.py have passed on the freshly
extracted archive.

The generator also re-checks the version entry (C4) and prints the embedded ROM
CRC so a wrong-ROM archive can never silently mint a golden constant. It refuses
to write the header if the entry count or version entry are wrong (unless
--force).

Usage:
  gen_expected_header.py --archive generic.o2r --out port/gen/gdx_o2r_expected.h
"""

import argparse
import os
import sys

import o2r_common as oc

HEADER_TEMPLATE = """\
/*
 * {basename}
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_expected_header.py
 * Source     : deterministic gdx-extract output ({source_name})
 * Source SHA : {sha256}
 * Entry count: {count}
 * Version CRC: 0x{crc:08X} (US rev0, P0 contract C4)
 *
 * Golden constants for first-boot validation (P0 contract C3/C5). The archive
 * SHA-256 is a compile-time constant ONLY because extraction is deterministic
 * and the ROM is hash-validated. Regenerate via the o2r_harness gauntlet after
 * any change to the extractor, recipes, or ROM profile.
 */
#ifndef GDX_O2R_EXPECTED_H
#define GDX_O2R_EXPECTED_H

#define {macro_sha} "{sha256}"
#define {macro_count} {count}

#endif /* GDX_O2R_EXPECTED_H */
"""


def main(argv):
    ap = argparse.ArgumentParser(description="Emit port/gen/gdx_o2r_expected.h.")
    ap.add_argument("--archive", required=True, help="validated deterministic .o2r")
    ap.add_argument(
        "--out",
        default=os.path.join(
            os.path.dirname(__file__), "..", "..", "port", "gen", "gdx_o2r_expected.h"
        ),
        help="output header path (default: port/gen/gdx_o2r_expected.h)",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="write the header even if entry-count / version checks fail (NOT recommended)",
    )
    args = ap.parse_args(argv)

    if not os.path.isfile(args.archive):
        print("ERROR: archive not found: %s" % args.archive, file=sys.stderr)
        return 2

    count = oc.record_count(args.archive)  # dup-inclusive central-directory records (C3)
    sha = oc.sha256_file(args.archive)
    ver = oc.check_version_entry(args.archive)

    print("archive     : %s" % args.archive)
    print("entry count : %d (C3 expects %d)" % (count, oc.EXPECTED_ENTRY_COUNT))
    print("sha256      : %s" % sha)
    if ver.get("crc") is not None:
        print("version CRC : 0x%08X  raw=%s  (%s)" % (ver["crc"], ver["raw_hex"], ver["reason"]))
    else:
        print("version     : %s" % ver["reason"])

    problems = []
    if count != oc.EXPECTED_ENTRY_COUNT:
        problems.append(
            "entry count %d != C3 expected %d" % (count, oc.EXPECTED_ENTRY_COUNT)
        )
    if not ver["ok"]:
        problems.append("version entry check failed: %s" % ver["reason"])

    if problems and not args.force:
        print("", file=sys.stderr)
        for p in problems:
            print("REFUSING to write header: %s" % p, file=sys.stderr)
        print("(use --force to override; this is almost always a bug)", file=sys.stderr)
        return 1

    crc = ver.get("crc") or 0
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)

    content = HEADER_TEMPLATE.format(
        basename=os.path.basename(args.out),
        source_name=os.path.basename(args.archive),
        sha256=sha,
        count=count,
        crc=crc,
        macro_sha=oc.MACRO_SHA256,
        macro_count=oc.MACRO_ENTRY_COUNT,
    )
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)

    print("")
    print("Wrote %s" % args.out)
    if problems:
        print("WARNING: written under --force despite: %s" % "; ".join(problems))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
