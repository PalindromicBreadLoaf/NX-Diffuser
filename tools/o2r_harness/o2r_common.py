#!/usr/bin/env python3
"""Shared helpers for the O2R extraction harness.

Stdlib only, deterministic. Every script in tools/o2r_harness/ imports this so
the archive-parsing logic (entry inventory, CRCs, version entry, family counts)
lives in exactly one place and stays consistent across the gauntlet.

Contract references (docs/investigation/2026-07-18/o2r-migration/P0_CONTRACTS.md):
  C3 - output contract (entry count, expected-header macros)
  C4 - version-entry contract ([0x01 big][u32 ROM CRC = 0x78D90EB3])
  C5 - validation-before-install checks 2-4
"""

import hashlib
import struct
import zipfile

# --- C4 constants (US rev0 profile) ---------------------------------------
# Torch stamps generic.o2r's `version` entry as [endianness u8][u32 ROM CRC].
# libultraship Archive.cpp reads: [endianness u8] then ReadUInt32() (big-endian).
VERSION_ENTRY_NAME = "version"
VERSION_ENDIANNESS_BIG = 0x01          # libultraship "big" marker
EXPECTED_ROM_CRC = 0x78D90EB3          # US rev0 cartridge CRC (C4)
VERSION_ENTRY_LEN = 5                  # 1 endianness byte + 4 CRC bytes

# C3 golden entry count (full recipe output including the inert families).
EXPECTED_ENTRY_COUNT = 3576  # deduplicated (Torch parity fix 2026-07-18); was 4240 with the Windows double-emit bug

# Macro names are a frozen code-level contract with agent 1-B (C3).
MACRO_SHA256 = "GDX_O2R_EXPECTED_SHA256"
MACRO_ENTRY_COUNT = "GDX_O2R_EXPECTED_ENTRY_COUNT"


def sha256_file(path):
    """SHA-256 of a file's raw bytes (the whole .o2r container)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def family_of(name):
    """Top-level category for an entry key.

    Keys are exact `category/symbol` strings; the metadata entries `version`
    and `portVersion` have no slash and are their own family.
    """
    return name.split("/", 1)[0] if "/" in name else name


class Entry:
    """One central-directory record, payload deferred until needed."""

    __slots__ = ("name", "crc", "size", "compress_type", "index")

    def __init__(self, name, crc, size, compress_type, index):
        self.name = name
        self.crc = crc                    # zip CRC-32 of the uncompressed payload
        self.size = size                  # uncompressed payload size
        self.compress_type = compress_type
        self.index = index                # position in the central directory


def read_records(path):
    """Return the ordered list of Entry records from the central directory.

    Includes DUPLICATE names: the legacy generic.o2r carries 664 duplicate-named
    zip records (same key emitted by multiple recipes, identical payloads). The
    "entry count" in C3 (4,240) is the number of central-directory RECORDS, not
    the number of unique names -- so counting must go through this list, never a
    name-keyed dict (which would collapse duplicates).
    """
    records = []
    with zipfile.ZipFile(path) as z:
        for i, info in enumerate(z.infolist()):
            records.append(
                Entry(
                    info.filename,
                    info.CRC & 0xFFFFFFFF,
                    info.file_size,
                    info.compress_type,
                    i,
                )
            )
    return records


def record_count(path):
    """Number of central-directory records (the C3 entry count, dup-inclusive)."""
    with zipfile.ZipFile(path) as z:
        return len(z.infolist())


def read_entries(path):
    """Return {name: Entry} (last record wins on duplicates).

    Convenience for by-name lookups; NOT for counting (see read_records).
    """
    out = {}
    for e in read_records(path):
        out[e.name] = e
    return out


def read_order(path):
    """Return the ordered list of entry names (central-directory order, with dups)."""
    with zipfile.ZipFile(path) as z:
        return z.namelist()


def read_payload(path, name):
    """Decompressed payload bytes for a single entry."""
    with zipfile.ZipFile(path) as z:
        return z.read(name)


def family_counts(path):
    """Return {family: count} for every top-level category in the archive."""
    counts = {}
    for name in read_order(path):
        fam = family_of(name)
        counts[fam] = counts.get(fam, 0) + 1
    return counts


def parse_version_entry(data):
    """Parse a `version` entry per C4.

    Returns dict: {ok, reason, endianness, crc, raw_hex}.
    ok is True only when the entry is exactly [0x01][u32 BE == EXPECTED_ROM_CRC].
    """
    result = {
        "ok": False,
        "reason": "",
        "endianness": None,
        "crc": None,
        "raw_hex": data.hex(),
    }
    if len(data) != VERSION_ENTRY_LEN:
        result["reason"] = (
            "version entry is %d bytes, expected %d ([endianness u8][u32 CRC])"
            % (len(data), VERSION_ENTRY_LEN)
        )
        return result
    endian = data[0]
    result["endianness"] = endian
    if endian != VERSION_ENDIANNESS_BIG:
        result["reason"] = (
            "endianness byte 0x%02X, expected 0x%02X (big)"
            % (endian, VERSION_ENDIANNESS_BIG)
        )
        return result
    crc = struct.unpack(">I", data[1:5])[0]
    result["crc"] = crc
    if crc != EXPECTED_ROM_CRC:
        result["reason"] = (
            "ROM CRC 0x%08X, expected 0x%08X (US rev0)" % (crc, EXPECTED_ROM_CRC)
        )
        return result
    result["ok"] = True
    result["reason"] = "ok"
    return result


def check_version_entry(path):
    """Read and validate the `version` entry of an archive (C4). Returns parse dict.

    Adds `present` key; if absent, ok=False with a reason.
    """
    entries = read_entries(path)
    if VERSION_ENTRY_NAME not in entries:
        return {
            "ok": False,
            "present": False,
            "reason": "no `version` entry in archive",
            "endianness": None,
            "crc": None,
            "raw_hex": "",
        }
    data = read_payload(path, VERSION_ENTRY_NAME)
    parsed = parse_version_entry(data)
    parsed["present"] = True
    return parsed
