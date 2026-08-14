#!/usr/bin/env python3
"""Reject a Switch release payload that would break the device or the updater.

    python3 tools/check_switch_package.py --version x.y.z dist/G-Diffuser-vx.y.z-switch
"""

import argparse
import hashlib
import os
import re
import struct
import sys

PAYLOAD = {
    "G-Diffuser.nro": 4 * 1024 * 1024,
    "gdiffuser.o2r": 16 * 1024,
}

USER_OWNED_NAMES = {
    "fzerox.o2r",
    "generic.o2r",
    "fzerox-disk.o2r",
    "n64ddipl.o2r",
    "fzerox.sav",
    "gdiffuser.cfg.json",
    "gdx_firstboot.cfg",
}
USER_OWNED_SUFFIXES = (".z64", ".n64", ".v64", ".ndd", ".sav", ".gdd", ".gdg")

NACP_NAME_OFFSET = 0x0000
NACP_AUTHOR_OFFSET = 0x0200
NACP_VERSION_OFFSET = 0x3060
NACP_TITLE_ENTRY_SIZE = 0x300


class CheckError(Exception):
    pass


def read_nacp(nro_path):
    """Return (name, author, version) from the NRO's embedded asset section."""
    with open(nro_path, "rb") as handle:
        data = handle.read()

    if len(data) < 0x80 or data[0x10:0x14] != b"NRO0":
        raise CheckError(f"{nro_path}: no NRO0 magic at 0x10")

    nro_size = struct.unpack_from("<I", data, 0x18)[0]
    if nro_size > len(data):
        raise CheckError(f"{nro_path}: header claims {nro_size} bytes, file holds {len(data)}")
    if data[nro_size:nro_size + 4] != b"ASET":
        raise CheckError(f"{nro_path}: no asset section")

    nacp_off, nacp_size = struct.unpack_from("<QQ", data, nro_size + 0x18)
    start = nro_size + nacp_off
    if nacp_size < 0x4000 or start + nacp_size > len(data):
        raise CheckError(f"{nro_path}: asset section points outside the file")

    nacp = data[start:start + nacp_size]

    def field(offset, size):
        return nacp[offset:offset + size].split(b"\0")[0].decode("utf-8", "replace")

    return (
        field(NACP_NAME_OFFSET, NACP_TITLE_ENTRY_SIZE),
        field(NACP_AUTHOR_OFFSET, 0x100),
        field(NACP_VERSION_OFFSET, 0x10),
    )


def check_version(stage, version):
    """Prove the tag, the NACP and the compiled constant are the same number."""
    nro = os.path.join(stage, "G-Diffuser.nro")
    name, author, nacp_version = read_nacp(nro)
    print(f"  NACP: {name!r} by {author!r}, version {nacp_version!r}")

    if name != "G-Diffuser":
        raise CheckError(f"NACP name is {name!r}, expected 'G-Diffuser'")
    if nacp_version != version:
        raise CheckError(
            f"NACP version is {nacp_version!r} but the release is {version!r}. "
            "Configure with -DGDX_VERSION=<version> and rebuild.")

    with open(nro, "rb") as handle:
        blob = handle.read()
    image = blob[:struct.unpack_from("<I", blob, 0x18)[0]]
    if image.count(version.encode() + b"\0") == 0:
        raise CheckError(
            f"the string {version!r} does not appear in {nro} at all, so the binary was not "
            "compiled with GDX_VERSION=" + version)


def check_manifest(stage):
    manifest_path = os.path.join(stage, "SHA256SUMS.txt")
    if not os.path.isfile(manifest_path):
        raise CheckError(f"{manifest_path} is missing")

    listed = {}
    with open(manifest_path, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            match = re.fullmatch(r"([0-9a-f]{64})\s[\s*](\S.*)", line)
            if not match:
                raise CheckError(f"{manifest_path}:{lineno}: not in sha256sum format: {line!r}")
            digest, name = match.group(1), match.group(2)
            if any(sep in name for sep in "/\\:"):
                raise CheckError(
                    f"{manifest_path}:{lineno}: {name!r} is a path")
            if name in listed:
                raise CheckError(f"{manifest_path}:{lineno}: {name!r} is listed twice")
            listed[name] = digest

    if set(listed) != set(PAYLOAD):
        extra = sorted(set(listed) - set(PAYLOAD))
        missing = sorted(set(PAYLOAD) - set(listed))
        raise CheckError(
            f"{manifest_path} must name exactly {sorted(PAYLOAD)}; "
            f"extra={extra} missing={missing}")

    for name, expected in sorted(listed.items()):
        path = os.path.join(stage, name)
        if not os.path.isfile(path):
            raise CheckError(f"{manifest_path} names {name}, which is not in the payload")
        size = os.path.getsize(path)
        if size < PAYLOAD[name]:
            raise CheckError(f"{name} is {size} bytes, below the {PAYLOAD[name]}-byte floor")
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != expected:
            raise CheckError(f"{name}: manifest says {expected}, bytes hash to {actual}")
        print(f"  {name}: {size} bytes, sha256 verified")


def check_no_user_files(stage):
    for root, _dirs, files in os.walk(stage):
        for name in files:
            lowered = name.lower()
            if lowered in USER_OWNED_NAMES or lowered.endswith(USER_OWNED_SUFFIXES):
                relative = os.path.relpath(os.path.join(root, name), stage)
                raise CheckError(
                    f"{relative} belongs to the user")


def check_shader_archive(stage):
    path = os.path.join(stage, "gdiffuser.o2r")
    with open(path, "rb") as handle:
        if handle.read(2) != b"PK":
            raise CheckError(f"{path} is not an archive")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stage", help="the staged payload directory")
    parser.add_argument("--version", required=True, help="release version, X.Y.Z, without the v")
    args = parser.parse_args()

    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        print(f"error: --version must be X.Y.Z, got {args.version!r}", file=sys.stderr)
        return 2
    if not os.path.isdir(args.stage):
        print(f"error: {args.stage} is not a directory", file=sys.stderr)
        return 2

    print(f"checking {args.stage} as version {args.version}")
    try:
        for name in sorted(PAYLOAD):
            if not os.path.isfile(os.path.join(args.stage, name)):
                raise CheckError(f"{name} is missing from the payload")
        check_no_user_files(args.stage)
        check_shader_archive(args.stage)
        check_version(args.stage, args.version)
        check_manifest(args.stage)
    except CheckError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print("payload OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
