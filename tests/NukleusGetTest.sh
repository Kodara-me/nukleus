#!/bin/sh
# Checks scripts/get-nukleus.sh against a fake release, with stub curl, cmake, ctest, git and
# sudo that only log their calls: nothing is built or installed and the network is not used.
# Usage: NukleusGetTest.sh <path to get-nukleus.sh>
set -u

script=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stubs="$work/stubs"
mkdir -p "$stubs"
export NUKLEUS_RELEASES_URL=https://releases.test FAKE="$work"

# release <dir>: a source archive and its checksum, as the release workflow attaches them.
release() {
    dir="$work/release/$1"
    mkdir -p "$dir" "$work/source/nukleus-0.1.0/scripts"
    printf 'project(Nukleus VERSION 0.1.0)\n' >"$work/source/nukleus-0.1.0/CMakeLists.txt"
    printf '#!/bin/sh\n' >"$work/source/nukleus-0.1.0/scripts/install-nukleus.sh"
    chmod +x "$work/source/nukleus-0.1.0/scripts/install-nukleus.sh"
    tar -czf "$dir/nukleus-0.1.0.tar.gz" -C "$work/source" nukleus-0.1.0
    (cd "$dir" && sha256sum nukleus-0.1.0.tar.gz >SHA256SUMS)
}
release latest/download
release download/v0.1.0

cat >"$stubs/curl" <<'EOF'
#!/bin/sh
# curl -fsSL -o <file> <url>: copies the file the fake release has at that URL.
out=$3 url=$4
cp "$FAKE/release/${url#https://releases.test/}" "$out" 2>/dev/null || exit 22
EOF
for tool in cmake ctest git sudo; do
    printf '#!/bin/sh\necho "%s $*" >>"$FAKE/log"\n[ "${FAIL_TOOL:-}" != %s ]\n' "$tool" "$tool" >"$stubs/$tool"
done
chmod +x "$stubs"/*
# Only the stubs and these tools are on the PATH: a real sudo, CMake or network is never used.
for tool in sh sed grep cut head cat cp rm mkdir mktemp sha256sum tar gzip uname printf; do
    real=$(command -v "$tool") && ln -s "$real" "$stubs/$tool"
done

# Without a controlling terminal, as under CI: /dev/tty cannot be opened.
setsid=$(command -v setsid) || { echo "setsid is needed"; exit 1; }

failures=0
# check <name> <expected exit> <log must contain, or ""> <log must not contain, or ""> <args…>
check() {
    name=$1 code=$2 present=$3 absent=$4
    shift 4
    : >"$work/log"
    output=$(PATH="$stubs" "$setsid" -w sh "$script" "$@" </dev/null 2>&1)
    actual=$?
    log=$(cat "$work/log")
    if [ "$actual" -ne "$code" ]; then
        echo "FAIL: $name: exit $actual, expected $code"; echo "$output"; failures=$((failures + 1))
    elif [ -n "$present" ] && ! printf '%s' "$log" | grep -qF -- "$present"; then
        echo "FAIL: $name: no call to: $present"; echo "$log"; failures=$((failures + 1))
    elif [ -n "$absent" ] && printf '%s' "$log" | grep -qF -- "$absent"; then
        echo "FAIL: $name: unexpected call to: $absent"; echo "$log"; failures=$((failures + 1))
    else
        echo "PASS: $name"
    fi
}

check "installs the latest release after its tests" 0 "/scripts/install-nukleus.sh --binary" "" -- --listen 127.0.0.1
check "forwards installer options" 0 "nukleus-manager --listen 127.0.0.1 --desktop-shortcut" "" -- --listen 127.0.0.1 --desktop-shortcut
check "tests before installing" 0 "ctest --test-dir" "" -- --listen 127.0.0.1
check "installs a chosen version" 0 "sudo " "" --version v0.1.0
check "dry run installs nothing" 0 "cmake --build" "sudo " --dry-run
check "unknown version fails" 1 "" "sudo " --version v9.9.9
check "unknown option fails" 1 "" "cmake" --listen 127.0.0.1
FAIL_TOOL=ctest check "failed tests install nothing" 1 "ctest" "sudo "
export FAIL_TOOL=

printf 'tampered' >>"$work/release/latest/download/nukleus-0.1.0.tar.gz"
check "checksum mismatch installs nothing" 1 "" "cmake"

[ "$failures" -eq 0 ]
