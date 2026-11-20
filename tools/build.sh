#!/usr/bin/env bash
# Builds the Z1 firmware, unsigned, into build/z1/. Runs in the ESP-IDF
# environment if idf.py is on PATH (CI's container), else in podman.
#
#   tools/build.sh [idf.py args...]               (default: build)
#   tools/build.sh --release <version> <out dir>
#
# --release builds from scratch, checks the version and stages
# <out dir>/open-z1-<version>-z1-unsigned/: flash.py, INSTALL.txt, flash_args
# and its images, flat. Official releases are this, signed by the maintainer.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v idf.py >/dev/null; then
    vols=(-v "$root:/project") args=("$@")
    if [[ "${1:-}" == --release && $# -eq 3 ]]; then
        mkdir -p "$3"
        vols+=(-v "$(cd "$3" && pwd):/out") args=(--release "$2" /out)
    fi
    exec podman run --rm "${vols[@]}" -w /project "${IDF_IMAGE:-docker.io/espressif/idf:v6.1}" \
        tools/build.sh "${args[@]}"
fi
cd "$root"

# idf.py applies sdkconfig.defaults and new Kconfig defaults only to a new
# sdkconfig; regenerate it whenever one of them changed (so `menuconfig`
# edits are lost: settings belong in sdkconfig.defaults or Kconfig).
if [[ -f build/z1/sdkconfig && -n "$(find sdkconfig.defaults components main \
        \( -name sdkconfig.defaults -o -name 'Kconfig*' \) -newer build/z1/sdkconfig)" ]]; then
    echo "defaults changed; regenerating build/z1/sdkconfig"
    rm -f build/z1/sdkconfig
fi

if [[ "${1:-}" != --release ]]; then
    [[ $# -gt 0 ]] || set -- build
    exec idf.py -B build/z1 -D SDKCONFIG=build/z1/sdkconfig "$@"
fi

version="${2:-}" out="${3:-}"
[[ -n "$version" && -n "$out" && $# -eq 3 ]] || { echo "usage: $0 --release <version> <out dir>" >&2; exit 2; }
want="$(sed -n 's/^CONFIG_APP_PROJECT_VER="\(.*\)"$/\1/p' sdkconfig.defaults)"
[[ "$want" == "$version" ]] || { echo "CONFIG_APP_PROJECT_VER is \"$want\", not \"$version\"" >&2; exit 1; }
rm -rf build/z1  # a clean build from the defaults alone
idf.py -B build/z1 -D SDKCONFIG=build/z1/sdkconfig build

mkdir -p "$out"
dir="$(cd "$out" && pwd)/open-z1-$version-z1-unsigned"
rm -rf "$dir"
mkdir "$dir"
cp flash.py "$dir/"
cp tools/release-INSTALL.txt "$dir/INSTALL.txt"
head -n1 build/z1/flash_args > "$dir/flash_args"
while read -r offset file; do
    cp "build/z1/$file" "$dir/"
    echo "$offset $(basename "$file")" >> "$dir/flash_args"
done < <(tail -n +2 build/z1/flash_args)
echo "staged $dir"
