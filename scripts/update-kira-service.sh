#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$project_root/build"
cmake_args=()
installer_args=()

usage() {
    cat <<'HELP'
Usage: scripts/update-kira-service.sh [options] [-- installer-options...]

Run on the Linux service host as the checkout owner (also suitable over SSH).
Fetch and fast-forward the current branch from its configured upstream,
configure, build, test, then invoke the systemd installer with sudo -n.

Options:
  --build-dir PATH    Build directory (default: <checkout>/build)
  --cmake-arg ARG     Extra CMake configure argument; may be repeated
  -h, --help          Show this help

Installer options after -- are passed to install-kira-service.sh.
Existing CMake cache settings are retained. New builds use Ninja and Release.
Requires a clean tracked working tree, an upstream branch, build dependencies,
and cached or passwordless sudo credentials. Does not update sibling checkouts.
HELP
}

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

while (( $# > 0 )); do
    case "$1" in
    --build-dir|--cmake-arg)
        (( $# >= 2 )) && [[ -n "$2" ]] || fail "$1 requires a value."
        if [[ "$1" == --build-dir ]]; then build_dir="$2"; else cmake_args+=("$2"); fi
        shift 2 ;;
    --) shift; installer_args=("$@"); break ;;
    -h|--help) usage; exit 0 ;;
    *) fail "Unknown option: $1" ;;
    esac
done

[[ "$(uname -s)" == Linux ]] || fail 'Systemd updates require Linux.'
(( EUID != 0 )) || fail 'Run as the checkout owner, without sudo; only installation uses sudo.'
cd -- "$project_root"
[[ "$build_dir" == /* ]] || build_dir="$project_root/$build_dir"
for tool in git cmake ctest sudo; do
    command -v "$tool" >/dev/null || fail "Required command is missing: $tool"
done
[[ "$(git rev-parse --show-toplevel)" == "$project_root" ]] || fail 'The script must be in the Kira Service checkout.'
git symbolic-ref --quiet HEAD >/dev/null || fail 'Check out a branch before updating.'
git rev-parse --verify '@{upstream}' >/dev/null 2>&1 || fail 'The current branch needs a configured upstream.'
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || fail 'Commit or stash tracked changes before updating.'

printf 'Fetching and updating Kira Service…\n'
git fetch --prune
git pull --ff-only

configure_args=()
if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    configure_args=(-G Ninja -DCMAKE_BUILD_TYPE=Release)
fi
printf 'Configuring, building and testing…\n'
cmake -S "$project_root" -B "$build_dir" "${configure_args[@]}" "${cmake_args[@]}"
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

printf 'Installing and restarting Kira Service…\n'
sudo -n "$project_root/scripts/install-kira-service.sh" "${installer_args[@]}" \
    --binary "$build_dir/src/kira-service" \
    --manager-binary "$build_dir/src/kira-service-manager"
