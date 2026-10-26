#!/usr/bin/env bash

set -euo pipefail

build_root="$1"
stage_root="$(mktemp -d)"
trap 'rm -rf "$stage_root"' EXIT

cmake --install "$build_root" --prefix "$stage_root"

daemon="$stage_root/libexec/nukleus/nukleusd"
manager="$stage_root/bin/nukleus-manager"
web="$stage_root/libexec/nukleus/nukleus-web"

[[ -x "$daemon" ]]
[[ -x "$manager" ]]
[[ -x "$web" ]]
