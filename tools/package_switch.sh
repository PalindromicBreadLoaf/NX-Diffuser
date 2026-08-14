#!/usr/bin/env bash
# Assemble a Nintendo Switch release zip from a finished build tree.

set -euo pipefail

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "usage: $0 <build-dir> <version> [output-dir]" >&2
    exit 2
fi

build_dir=$1
version=$2
out_dir=${3:-dist}
repo_root=$(cd "$(dirname "$0")/.." && pwd)

binary_dir="$build_dir/port"
if [ ! -f "$binary_dir/G-Diffuser.nro" ]; then
    echo "error: no G-Diffuser.nro in $binary_dir" >&2
    echo "       nx_create_nro hangs the NRO off its own target, so build the default target," >&2
    echo "       or ask for it by name: cmake --build $build_dir --target G-Diffuser_nro" >&2
    exit 1
fi

case "$version" in
    *-switch) release_name="G-Diffuser-v$version" ;;
    *)        release_name="G-Diffuser-v$version-switch" ;;
esac

stage="$out_dir/$release_name"
rm -rf "$stage"
mkdir -p "$stage"

payload=(
    G-Diffuser.nro
    gdiffuser.o2r
)
extras=(
    gamecontrollerdb.txt
    fonts
    LICENSE
    THIRD_PARTY_NOTICES.md
    LICENSES
)

for item in "${payload[@]}" "${extras[@]}"; do
    if [ ! -e "$binary_dir/$item" ]; then
        echo "error: build tree is missing $item" >&2
        exit 1
    fi
    cp -r "$binary_dir/$item" "$stage/"
done

(cd "$stage" && sha256sum "${payload[@]}" > SHA256SUMS.txt)

python3 "$repo_root/tools/check_switch_package.py" --version "$version" "$stage"

archive="$out_dir/$release_name.zip"
rm -f "$archive"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}" python3 - "$stage" "$archive" <<'PY'
import os, sys, time, zipfile

stage, archive = sys.argv[1], sys.argv[2]
root = os.path.dirname(stage) or "."
stamp = time.gmtime(max(int(os.environ.get("SOURCE_DATE_EPOCH", "0")), 315532800))[:6]

entries = []
for dirpath, dirnames, filenames in os.walk(stage):
    dirnames.sort()
    for name in sorted(filenames):
        full = os.path.join(dirpath, name)
        entries.append((os.path.relpath(full, root).replace(os.sep, "/"), full))

with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
    for arcname, full in sorted(entries):
        info = zipfile.ZipInfo(arcname, date_time=stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(full, "rb") as handle:
            zf.writestr(info, handle.read())
print(f"{len(entries)} entries")
PY

echo
echo "$archive"
sha256sum "$archive"
