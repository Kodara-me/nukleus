#!/bin/sh
# Builds, tests and installs a Nukleus release as a Linux systemd service:
#
#   curl -fsSL https://github.com/jstmichel/nukleus/releases/latest/download/get-nukleus.sh | sh
#
# Usage: get-nukleus.sh [--version <tag>] [--dry-run] [--help] [-- <installer options>]
#
#   --version <tag>  install this release (for example v0.1.0) instead of the latest
#   --dry-run        download, check and build, then print the installation command
#   -- <options>     passed to scripts/install-nukleus.sh, for example
#                    -- --listen 127.0.0.1   or   -- --desktop-shortcut
#
# Requires curl, tar, git, CMake 3.24+, a C++20 compiler, Qt 6.2+ with Network, WebSockets,
# Widgets, SVG and LinguistTools, LibArchive, and sudo. Ardoise is used when installed, otherwise CMake
# clones it. The release is installed only after its tests pass. Exit codes: 0 installed,
# 1 failure.

set -u

releases=${NUKLEUS_RELEASES_URL:-https://github.com/jstmichel/nukleus/releases}
tag=""
dry_run=0

usage() {
    sed -n '2,16s/^# \{0,1\}//p' "$0" 2>/dev/null \
        || echo "Usage: get-nukleus.sh [--version <tag>] [--dry-run] [-- <installer options>]"
}

say() {
    printf '%s\n' "$*"
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
        --version)
            [ $# -ge 2 ] || fail "--version needs a tag, for example v0.1.0"
            tag=$2
            shift 2
            ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) fail "unknown option: $1 (installer options go after --; see --help)" ;;
    esac
done
# The remaining arguments are the installer's.

# sha256 <file>: prints the file's SHA-256 digest.
sha256() {
    sha256sum "$1" | cut -d ' ' -f 1
}

[ "$(uname -s)" = Linux ] || fail "Nukleus installs as a Linux systemd service"
for tool in curl tar git cmake ctest mktemp sha256sum sudo; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is needed"
done

work=$(mktemp -d) || fail "cannot create a temporary folder"
trap 'rm -rf "$work"' EXIT
trap 'exit 1' INT TERM

if [ -n "$tag" ]; then
    base="$releases/download/$tag"
else
    base="$releases/latest/download"
fi

say "Looking for ${tag:-the latest release}…"
curl -fsSL -o "$work/SHA256SUMS" "$base/SHA256SUMS" || fail "cannot download $base/SHA256SUMS"
archive=$(sed -n 's/^[0-9a-f]\{64\}  \(nukleus-[0-9][0-9.]*\.tar\.gz\)$/\1/p' "$work/SHA256SUMS" | head -n 1)
[ -n "$archive" ] || fail "the release lists no source archive"
version=${archive#nukleus-}
version=${version%.tar.gz}

say "Downloading $archive…"
curl -fsSL -o "$work/$archive" "$base/$archive" || fail "cannot download $base/$archive"
expected=$(sed -n "s/^\([0-9a-f]\{64\}\)  $archive\$/\1/p" "$work/SHA256SUMS")
[ "$(sha256 "$work/$archive")" = "$expected" ] \
    || fail "$archive does not match the release's SHA256SUMS; nothing was installed"
say "Checksum verified."

tar -xzf "$work/$archive" -C "$work" || fail "cannot extract $archive"
source="$work/nukleus-$version"
[ -f "$source/CMakeLists.txt" ] && [ -x "$source/scripts/install-nukleus.sh" ] \
    || fail "$archive has no nukleus-$version/CMakeLists.txt and scripts/install-nukleus.sh"

build="$work/build"
generator_args=""
if command -v ninja >/dev/null 2>&1; then
    generator_args="-G Ninja"
fi

say "Building Nukleus $version…"
# shellcheck disable=SC2086 # generator_args is empty or two words.
cmake -S "$source" -B "$build" -DCMAKE_BUILD_TYPE=Release $generator_args \
    || fail "CMake could not configure Nukleus; check that Qt 6.2+ and CMake 3.24+ are installed"
cmake --build "$build" --parallel || fail "Nukleus did not build"
say "Testing Nukleus $version…"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build" --output-on-failure \
    || fail "the Nukleus tests failed; nothing was installed"

set -- "$source/scripts/install-nukleus.sh" --binary "$build/src/nukleusd" \
    --manager-binary "$build/src/nukleus-manager" "$@"

if [ "$dry_run" -eq 1 ]; then
    say "  would run: sudo $*"
    say "Dry run: nothing was installed."
    exit 0
fi

say "Installing Nukleus $version…"
# Under curl … | sh the standard input is the script, so the installer's questions
# (listen address, LAN passphrase) read the terminal instead, when there is one.
if (: </dev/tty) 2>/dev/null; then
    sudo "$@" </dev/tty || fail "the installer stopped; see the messages above"
else
    sudo "$@" </dev/null || fail "the installer stopped; without a terminal, pass -- --listen 127.0.0.1"
fi
