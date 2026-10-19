#!/usr/bin/env bash
set -euo pipefail

# Exercise the updater without network access, sudo, or a live installation.
fixture="$(mktemp -d)"
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/repo/scripts" "$fixture/bin"
cp "$1/scripts/update-nukleus.sh" "$fixture/repo/scripts/"
export UPDATE_TEST_ROOT="$fixture/repo" UPDATE_TEST_LOG="$fixture/log"
cat > "$fixture/bin/git" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
rev-parse) [[ "$2" != --show-toplevel ]] || printf '%s\n' "$UPDATE_TEST_ROOT" ;;
symbolic-ref) exit 0 ;;
status) [[ "${UPDATE_TEST_FAIL:-}" != dirty ]] || echo ' M README.md' ;;
fetch|pull)
    printf 'git %s\n' "$*" >> "$UPDATE_TEST_LOG"
    [[ "${UPDATE_TEST_FAIL:-}" != "$1" ]] ;;
*) exit 1 ;;
esac
MOCK
cat > "$fixture/bin/command-mock" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
name="${0##*/}"
printf '%s' "$name" >> "$UPDATE_TEST_LOG"
printf ' <%s>' "$@" >> "$UPDATE_TEST_LOG"
printf '\n' >> "$UPDATE_TEST_LOG"
[[ "${UPDATE_TEST_FAIL:-}" != "$name" ]]
MOCK
chmod +x "$fixture/bin/"*
for tool in cmake ctest sudo; do ln -s command-mock "$fixture/bin/$tool"; done
export PATH="$fixture/bin:$PATH"
updater="$fixture/repo/scripts/update-nukleus.sh"

bash "$updater" --build-dir 'build space' --cmake-arg '-DFETCHCONTENT_SOURCE_DIR_ARDOISE=/ui space' -- --no-enable
[[ "$(wc -l < "$UPDATE_TEST_LOG")" == 6 ]]
grep -F 'git fetch --prune' "$UPDATE_TEST_LOG"
grep -F 'git pull --ff-only' "$UPDATE_TEST_LOG"
grep -F '<-G> <Ninja> <-DCMAKE_BUILD_TYPE=Release> <-DFETCHCONTENT_SOURCE_DIR_ARDOISE=/ui space>' "$UPDATE_TEST_LOG"
grep -F "sudo <-n> <$UPDATE_TEST_ROOT/scripts/install-nukleus.sh> <--no-enable> <--binary> <$UPDATE_TEST_ROOT/build space/src/nukleusd> <--manager-binary> <$UPDATE_TEST_ROOT/build space/src/nukleus-manager>" "$UPDATE_TEST_LOG"

for failure in dirty fetch pull cmake ctest; do
    : > "$UPDATE_TEST_LOG"
    if UPDATE_TEST_FAIL="$failure" bash "$updater"; then
        echo "Expected failure for $failure" >&2; exit 1
    fi
    if grep -Eq '^sudo ' "$UPDATE_TEST_LOG"; then
        echo "Installation attempted after $failure failure" >&2; exit 1
    fi
done
mkdir -p "$UPDATE_TEST_ROOT/build"
touch "$UPDATE_TEST_ROOT/build/CMakeCache.txt"
: > "$UPDATE_TEST_LOG"
bash "$updater"
if grep -Eq '<Ninja>|<-DCMAKE_BUILD_TYPE=Release>' "$UPDATE_TEST_LOG"; then
    echo 'Existing build settings were overridden' >&2; exit 1
fi
