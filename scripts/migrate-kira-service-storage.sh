#!/usr/bin/env bash

set -euo pipefail

readonly CATEGORIES=(upscale llm companion)

source_dir=""
models_dir=""
config_dir=""
dry_run=false

usage() {
    cat <<'EOF'
Usage: scripts/migrate-kira-service-storage.sh --source DIR --models-dir DIR --config-dir DIR [--dry-run]

Copy a legacy Kira Service model tree into the split storage layout:
weights to MODELS_DIR/{generation,upscale,llm,companion}/ and the catalogue
(category metadata.json files and model-routing.json) to CONFIG_DIR/catalog/.

The source is never modified. Destinations must be absent or empty; nothing is
merged or overwritten. Files are staged beside each destination and moved into
place only after every copy succeeded and its size matches the source.

Options:
  --source DIR       Legacy model root (the former .../kira-service/models)
  --models-dir DIR   Destination weight directory
  --config-dir DIR   Destination configuration directory
  --dry-run          Print the planned copies without modifying the filesystem
  -h, --help         Show this help
EOF
}

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

while (($#)); do
    case "$1" in
        --source) (($# >= 2)) || fail "--source requires a directory."; source_dir="$2"; shift 2 ;;
        --models-dir) (($# >= 2)) || fail "--models-dir requires a directory."; models_dir="$2"; shift 2 ;;
        --config-dir) (($# >= 2)) || fail "--config-dir requires a directory."; config_dir="$2"; shift 2 ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1" ;;
    esac
done

[[ -n "$source_dir" && -n "$models_dir" && -n "$config_dir" ]] || { usage >&2; exit 2; }
[[ -d "$source_dir" ]] || fail "The source directory does not exist: $source_dir"
source_dir="$(cd -- "$source_dir" && pwd)"
models_dir="$(realpath -m -- "$models_dir")"
config_dir="$(realpath -m -- "$config_dir")"
catalog_dir="$config_dir/catalog"
stage_models="$models_dir/.kira-migration"
stage_catalog="$config_dir/.kira-migration-catalog"

for path in "$models_dir" "$config_dir"; do
    [[ "$path/" != "$source_dir/"* && "$source_dir/" != "$path/"* ]] || fail "Source and destination must not overlap: $path"
done

empty_or_absent() { [[ ! -e "$1" ]] || { [[ -d "$1" ]] && [[ -z "$(ls -A -- "$1")" ]]; }; }
for path in "$models_dir/generation" "${CATEGORIES[@]/#/$models_dir/}" "$catalog_dir"; do
    empty_or_absent "$path" || fail "The destination is not empty: $path"
done
for path in "$stage_models" "$stage_catalog"; do
    [[ ! -e "$path" ]] || fail "A previous migration left $path; inspect and remove it first."
done

# Each plan entry is "source<TAB>destination relative to its staging root<TAB>models|catalog".
plan=()
add() { plan+=("$1"$'\t'"$2"$'\t'"$3"); }

for entry in "$source_dir"/* "$source_dir"/.[!.]*; do
    [[ -e "$entry" ]] || continue
    name="$(basename -- "$entry")"
    case "$name" in
        metadata.json) add "$entry" "generation.json" catalog ;;
        model-routing.json) add "$entry" "model-routing.json" catalog ;;
        companions) add "$entry" "generation/companions" models ;;
        upscale|llm|companion)
            for item in "$entry"/* "$entry"/.[!.]*; do
                [[ -e "$item" ]] || continue
                if [[ "$(basename -- "$item")" == "metadata.json" ]]; then
                    add "$item" "$name.json" catalog
                else
                    add "$item" "$name/$(basename -- "$item")" models
                fi
            done ;;
        *)
            if [[ -f "$entry" ]]; then
                add "$entry" "generation/$name" models
            else
                printf 'Skipping unsupported entry: %s\n' "$entry"
            fi ;;
    esac
done
((${#plan[@]})) || fail "The source contains no models or catalogue files: $source_dir"

stage_root() { [[ "$1" == models ]] && printf '%s' "$stage_models" || printf '%s' "$stage_catalog"; }
final_root() { [[ "$1" == models ]] && printf '%s' "$models_dir" || printf '%s' "$catalog_dir"; }

for entry in "${plan[@]}"; do
    IFS=$'\t' read -r from to kind <<<"$entry"
    printf 'copy %s -> %s/%s\n' "$from" "$(final_root "$kind")" "$to"
done
if "$dry_run"; then
    printf 'Dry run: nothing was copied.\n'
    exit 0
fi

cleanup() { rm -rf -- "$stage_models" "$stage_catalog"; }
trap 'cleanup' ERR INT TERM

mkdir -p -- "$stage_models/generation" "${CATEGORIES[@]/#/$stage_models/}" "$stage_catalog"
for entry in "${plan[@]}"; do
    IFS=$'\t' read -r from to kind <<<"$entry"
    cp -R --preserve=mode,timestamps --reflink=auto -- "$from" "$(stage_root "$kind")/$to"
done

# Every regular file of the source must have been staged with the same size.
for entry in "${plan[@]}"; do
    IFS=$'\t' read -r from to kind <<<"$entry"
    while IFS= read -r -d '' file; do
        staged="$(stage_root "$kind")/$to${file#"$from"}"
        [[ -f "$staged" && "$(stat -c %s -- "$file")" == "$(stat -c %s -- "$staged")" ]] \
            || { cleanup; fail "Staged copy does not match the source: $file"; }
    done < <(find "$from" -type f -print0)
done

trap - ERR INT TERM
for category in generation "${CATEGORIES[@]}"; do
    [[ ! -d "$models_dir/$category" ]] || rmdir -- "$models_dir/$category"
    mv -- "$stage_models/$category" "$models_dir/$category"
done
rmdir -- "$stage_models"
[[ ! -d "$catalog_dir" ]] || rmdir -- "$catalog_dir"
mv -- "$stage_catalog" "$catalog_dir"

printf 'Migrated %d entries. The source was left unchanged: %s\n' "${#plan[@]}" "$source_dir"
