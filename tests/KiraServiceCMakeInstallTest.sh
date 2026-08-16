#!/usr/bin/env bash

set -euo pipefail

build_root="$1"
stage_root="$(mktemp -d)"
trap 'rm -rf "$stage_root"' EXIT

cmake --install "$build_root" --prefix "$stage_root"

daemon="$stage_root/libexec/kira-service/kira-service"
manager="$stage_root/bin/kira-service-manager"

[[ -x "$daemon" ]]
[[ -x "$manager" ]]
