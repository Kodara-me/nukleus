#!/usr/bin/env bash

set -euo pipefail

project_root="$1"
migrate="$project_root/scripts/migrate-kira-service-storage.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

source_dir="$work/backup/models"
mkdir -p "$source_dir/upscale" "$source_dir/llm" "$source_dir/companion" "$source_dir/edition" \
    "$source_dir/companions/legacy.safetensors/vae"
printf 'generation' > "$source_dir/legacy.safetensors"
printf 'attachment' > "$source_dir/companions/legacy.safetensors/vae/vae.safetensors"
printf '{"legacy.safetensors":{"displayName":"Legacy"}}' > "$source_dir/metadata.json"
printf '{"version":6}' > "$source_dir/model-routing.json"
printf 'upscaler' > "$source_dir/upscale/two-x.gguf"
printf '{}' > "$source_dir/upscale/metadata.json"
printf 'llm' > "$source_dir/llm/chat.gguf"
printf '{"chat.gguf":{}}' > "$source_dir/llm/metadata.json"
printf 'mmproj' > "$source_dir/companion/vision.gguf"
printf '{"vision.gguf":{}}' > "$source_dir/companion/metadata.json"
before="$(cd "$source_dir" && find . -printf '%p %s %T@\n' | sort)"

models="$work/srv/models/kira-service"
config="$work/srv/config/kira-service"

# A dry run only reports the plan.
"$migrate" --source "$source_dir" --models-dir "$models" --config-dir "$config" --dry-run > "$work/dry-run"
grep -Fq "copy $source_dir/legacy.safetensors -> $models/generation/legacy.safetensors" "$work/dry-run"
grep -Fq "copy $source_dir/upscale/metadata.json -> $config/catalog/upscale.json" "$work/dry-run"
grep -Fq "Skipping unsupported entry: $source_dir/edition" "$work/dry-run"
[[ ! -e "$work/srv" ]]

"$migrate" --source "$source_dir" --models-dir "$models" --config-dir "$config" > "$work/output"
cmp "$source_dir/legacy.safetensors" "$models/generation/legacy.safetensors"
cmp "$source_dir/companions/legacy.safetensors/vae/vae.safetensors" \
    "$models/generation/companions/legacy.safetensors/vae/vae.safetensors"
cmp "$source_dir/upscale/two-x.gguf" "$models/upscale/two-x.gguf"
cmp "$source_dir/llm/chat.gguf" "$models/llm/chat.gguf"
cmp "$source_dir/companion/vision.gguf" "$models/companion/vision.gguf"
cmp "$source_dir/metadata.json" "$config/catalog/generation.json"
cmp "$source_dir/upscale/metadata.json" "$config/catalog/upscale.json"
cmp "$source_dir/llm/metadata.json" "$config/catalog/llm.json"
cmp "$source_dir/companion/metadata.json" "$config/catalog/companion.json"
cmp "$source_dir/model-routing.json" "$config/catalog/model-routing.json"
[[ -z "$(find "$models" -name metadata.json -o -name model-routing.json)" ]]
[[ ! -e "$models/edition" && ! -e "$models/.kira-migration" && ! -e "$config/.kira-migration-catalog" ]]
[[ "$(cd "$source_dir" && find . -printf '%p %s %T@\n' | sort)" == "$before" ]]

# A second run never merges into the populated destinations.
if "$migrate" --source "$source_dir" --models-dir "$models" --config-dir "$config" 2> "$work/error"; then
    exit 1
fi
grep -q 'destination is not empty' "$work/error"

# A destination inside the source is refused.
if "$migrate" --source "$source_dir" --models-dir "$source_dir/out" --config-dir "$work/other" 2> "$work/error"; then
    exit 1
fi
grep -q 'must not overlap' "$work/error"

# Empty destination directories are accepted.
mkdir -p "$work/empty/models/generation" "$work/empty/config/catalog"
"$migrate" --source "$source_dir" --models-dir "$work/empty/models" --config-dir "$work/empty/config" > /dev/null
cmp "$source_dir/llm/chat.gguf" "$work/empty/models/llm/chat.gguf"

# The migrated tree is readable by the daemon's split layout.
[[ -d "$models/generation" && -d "$config/catalog" ]]
