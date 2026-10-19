#!/usr/bin/env bash

set -euo pipefail

project_root="$1"
service_binary="$2"
manager_binary="$3"
stage_root="$(mktemp -d)"
mock_bin="$(mktemp -d)"
interactive_root="$(mktemp -d)"
loopback_root="$(mktemp -d)"
storage_root="$(mktemp -d)"
trap 'rm -rf "$stage_root" "$mock_bin" "$interactive_root" "$loopback_root" "$storage_root" "$storage_root-output"' EXIT

printf '%s\n' '#!/usr/bin/env bash' \
    'printf "2: enp1s0    inet 10.0.0.93/24 brd 10.0.0.255 scope global enp1s0\\n3: wlan0    inet 192.168.1.20/24 brd 192.168.1.255 scope global wlan0\\n"' \
    > "$mock_bin/ip"
chmod +x "$mock_bin/ip"

"$project_root/scripts/install-nukleus.sh" \
    --root "$stage_root" \
    --binary "$service_binary" \
    --manager-binary "$manager_binary" \
    --user-bin /home/kira-test/.local/bin \
    --listen 10.0.0.93 \
    --token "correct horse battery staple" \
    --no-enable

program="$stage_root/usr/local/libexec/nukleus/nukleusd"
manager="$stage_root/home/kira-test/.local/bin/nukleus-manager"
config="$stage_root/etc/nukleus/nukleus.conf"
token="$stage_root/etc/nukleus/token"
unit="$stage_root/usr/lib/systemd/system/nukleus.service"

[[ -x "$program" ]]
[[ -x "$manager" ]]
[[ "$(stat -c '%a' "$config")" == "600" ]]
[[ "$(stat -c '%a' "$token")" == "640" ]]
[[ "$(stat -c '%a' "$unit")" == "644" ]]
grep -qx 'NUKLEUS_LISTEN=10.0.0.93' "$config"
grep -qx 'NUKLEUS_PORT=9384' "$config"
grep -qx 'NUKLEUS_TOKEN_FILE=/etc/nukleus/token' "$config"
grep -qx 'correct horse battery staple' "$token"
grep -Fqx 'ExecStart=/usr/local/libexec/nukleus/nukleusd --listen ${NUKLEUS_LISTEN} --port ${NUKLEUS_PORT} --token-file ${NUKLEUS_TOKEN_FILE}' "$unit"
! grep -q '@NUKLEUS_GPU_GROUPS@' "$unit"

replacement_binary="$stage_root/replacement-service"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$replacement_binary"
chmod +x "$replacement_binary"
mkdir -p "$stage_root/var/lib/nukleus/data/kira-canvas/models"
printf 'legacy model' > "$stage_root/var/lib/nukleus/data/kira-canvas/models/legacy.safetensors"
"$project_root/scripts/install-nukleus.sh" \
    --root "$stage_root" \
    --binary "$replacement_binary" \
    --manager-binary "$manager_binary" \
    --user-bin /home/kira-test/.local/bin \
    --listen 10.0.0.99 \
    --token "replace me" \
    --no-enable > "$stage_root/update-output"
cmp "$replacement_binary" "$program"
grep -qx 'NUKLEUS_LISTEN=10.0.0.93' "$config"
grep -qx 'correct horse battery staple' "$token"
grep -q '^Updated Nukleus\.$' "$stage_root/update-output"
[[ ! -e "$stage_root/var/lib/nukleus/data/kira-canvas/models" ]]
[[ "$(<"$stage_root/var/lib/nukleus/data/nukleus/models/legacy.safetensors")" == 'legacy model' ]]

mkdir -p "$stage_root/var/lib/nukleus/data/kira-canvas/models" "$stage_root/var/lib/nukleus/data/nukleus/models"
if "$project_root/scripts/install-nukleus.sh" --root "$stage_root" --binary "$replacement_binary" --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable; then
    printf 'Expected conflicting model roots to be rejected\n' >&2
    exit 1
fi
rm -rf "$stage_root/var/lib/nukleus/data/kira-canvas"

printf '\ninteractive LAN token\n' | PATH="$mock_bin:$PATH" script -qec \
    "'$project_root/scripts/install-nukleus.sh' --root '$interactive_root' --binary '$service_binary' --manager-binary '$manager_binary' --user-bin /home/kira-test/.local/bin --no-enable" /dev/null \
    > "$interactive_root-output"
grep -qx 'NUKLEUS_LISTEN=10.0.0.93' "$interactive_root/etc/nukleus/nukleus.conf"
grep -qx 'interactive LAN token' "$interactive_root/etc/nukleus/token"

printf '1\n' | PATH="$mock_bin:$PATH" script -qec \
    "'$project_root/scripts/install-nukleus.sh' --root '$loopback_root' --binary '$service_binary' --manager-binary '$manager_binary' --user-bin /home/kira-test/.local/bin --no-enable" /dev/null \
    > "$loopback_root-output"
grep -qx 'NUKLEUS_LISTEN=127.0.0.1' "$loopback_root/etc/nukleus/nukleus.conf"
[[ -z "$(tr -d '\n\r' < "$loopback_root/etc/nukleus/token")" ]]

mkdir -p "$stage_root/var/lib/nukleus/data"
"$project_root/scripts/uninstall-nukleus.sh" --root "$stage_root" --user-bin /home/kira-test/.local/bin --purge --yes
[[ ! -e "$program" ]]
[[ ! -e "$manager" ]]
[[ ! -e "$config" ]]
[[ ! -e "$stage_root/var/lib/nukleus" ]]

"$project_root/scripts/install-nukleus.sh" \
    --root "$stage_root" \
    --binary "$service_binary" \
    --manager-binary "$manager_binary" \
    --user-bin /home/kira-test/.local/bin \
    --listen 10.0.0.93 \
    --token "dry run token" \
    --no-enable \
    --dry-run
[[ ! -e "$stage_root/usr/local/libexec/nukleus/nukleusd" ]]

# Split storage: directories are created, configured and preserved by updates.
storage_install() {
    "$project_root/scripts/install-nukleus.sh" --root "$storage_root" --binary "$service_binary" \
        --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --listen 127.0.0.1 --no-enable "$@"
}
mkdir -p "$storage_root/home/kira-test/Bureau"
storage_install --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus \
    --desktop-dir /home/kira-test/Bureau > "$storage_root-output"
storage_config="$storage_root/etc/nukleus/nukleus.conf"

# The manager gets an application-menu entry, themed icons and a trusted desktop shortcut.
menu_entry="$storage_root/home/kira-test/.local/share/applications/nukleus-manager.desktop"
shortcut="$storage_root/home/kira-test/Bureau/nukleus-manager.desktop"
grep -Fqx 'Exec="/home/kira-test/.local/bin/nukleus-manager" --gui' "$menu_entry"
grep -qx 'Icon=/home/kira-test/.local/share/icons/hicolor/256x256/apps/nukleus-manager.png' "$menu_entry"
if grep -q '@NUKLEUS_MANAGER' "$menu_entry"; then exit 1; fi
[[ "$(stat -c '%a' "$menu_entry")" == "644" ]]
cmp "$menu_entry" "$shortcut"
[[ "$(stat -c '%a' "$shortcut")" == "755" ]]
for size in 16 32 48 64 128 256 512; do
    cmp "$project_root/assets/icons/nukleus-manager-$size.png" \
        "$storage_root/home/kira-test/.local/share/icons/hicolor/${size}x${size}/apps/nukleus-manager.png"
done
grep -Fq "Desktop shortcut: $shortcut" "$storage_root-output"
if command -v desktop-file-validate >/dev/null 2>&1; then desktop-file-validate "$menu_entry"; fi
storage_unit="$storage_root/usr/lib/systemd/system/nukleus.service"
for directory in generation upscale llm companion; do [[ -d "$storage_root/srv/lab/models/$directory" ]]; done
[[ -d "$storage_root/srv/lab/config/nukleus/catalog" ]]
grep -qx 'NUKLEUS_MODELS_DIR=/srv/lab/models' "$storage_config"
grep -qx 'NUKLEUS_CONFIG_DIR=/srv/lab/config/nukleus' "$storage_config"
grep -Fqx 'ExecStart=/usr/local/libexec/nukleus/nukleusd --listen ${NUKLEUS_LISTEN} --port ${NUKLEUS_PORT} --token-file ${NUKLEUS_TOKEN_FILE} --models-dir ${NUKLEUS_MODELS_DIR} --config-dir ${NUKLEUS_CONFIG_DIR}' "$storage_unit"
grep -qx 'RequiresMountsFor=/srv/lab/models /srv/lab/config/nukleus' "$storage_unit"
grep -qx 'ReadWritePaths=/var/lib/nukleus /run/nukleus /srv/lab/models /srv/lab/config/nukleus/catalog' "$storage_unit"
grep -qx 'ProtectSystem=strict' "$storage_unit"
if grep -q '@NUKLEUS_STORAGE' "$storage_unit"; then exit 1; fi

storage_install > /dev/null
grep -qx 'NUKLEUS_MODELS_DIR=/srv/lab/models' "$storage_config"
grep -q -- '--models-dir ${NUKLEUS_MODELS_DIR}' "$storage_unit"

storage_install --models-dir /srv/lab/other-models --config-dir /srv/lab/config/nukleus > /dev/null
grep -qx 'NUKLEUS_MODELS_DIR=/srv/lab/other-models' "$storage_config"
grep -qx 'NUKLEUS_LISTEN=127.0.0.1' "$storage_config"

for invalid in "--models-dir /srv/lab/models" "--models-dir relative --config-dir /srv/lab/config" "--models-dir /srv/with#hash --config-dir /srv/lab/config"; do
    # shellcheck disable=SC2086
    if storage_install $invalid > /dev/null 2>&1; then
        printf 'Expected invalid storage options to be rejected: %s\n' "$invalid" >&2
        exit 1
    fi
done

# The legacy unit renders no storage options.
legacy_unit="$loopback_root/usr/lib/systemd/system/nukleus.service"
if grep -q -e 'RequiresMountsFor' -e '--models-dir' -e '@NUKLEUS_STORAGE' "$legacy_unit"; then exit 1; fi
grep -qx 'ReadWritePaths=/var/lib/nukleus /run/nukleus' "$legacy_unit"

# The desktop shortcut is opt-in; uninstall removes the desktop files.
rm "$shortcut"
storage_install > "$storage_root-output"
if grep -q 'Desktop shortcut:' "$storage_root-output" || [[ -e "$shortcut" ]]; then exit 1; fi
[[ -f "$menu_entry" ]]
storage_install --desktop-dir /home/kira-test/Bureau > /dev/null
[[ -f "$shortcut" ]]
printf 'unrelated' > "$storage_root/home/kira-test/Bureau/other.desktop"
"$project_root/scripts/uninstall-nukleus.sh" --root "$storage_root" --user-bin /home/kira-test/.local/bin \
    --desktop-dir /home/kira-test/Bureau > /dev/null
[[ ! -e "$menu_entry" && ! -e "$shortcut" ]]
[[ ! -e "$storage_root/home/kira-test/.local/share/icons/hicolor/48x48/apps/nukleus-manager.png" ]]
[[ -f "$storage_root/home/kira-test/Bureau/other.desktop" && -d "$storage_root/srv/lab/models/generation" ]]

# A deployment made under the former kira-service name is moved, not merged.
legacy_root="$(mktemp -d)"
trap 'rm -rf "$stage_root" "$mock_bin" "$interactive_root" "$loopback_root" "$storage_root" "$storage_root-output" "$legacy_root"' EXIT
mkdir -p "$legacy_root/etc/kira-service" "$legacy_root/var/lib/kira-service/data/kira-service/models/llm" \
    "$legacy_root/usr/local/libexec/kira-service" "$legacy_root/usr/lib/systemd/system"
printf 'KIRA_SERVICE_LISTEN=10.0.0.93\nKIRA_SERVICE_PORT=9384\nKIRA_SERVICE_TOKEN_FILE=/etc/kira-service/token\nKIRA_SERVICE_MODELS_DIR=/srv/lab/models\nKIRA_SERVICE_CONFIG_DIR=/srv/lab/config\n' \
    > "$legacy_root/etc/kira-service/kira-service.conf"
printf 'legacy key\n' > "$legacy_root/etc/kira-service/token"
printf 'chat' > "$legacy_root/var/lib/kira-service/data/kira-service/models/llm/chat.gguf"
printf 'old' > "$legacy_root/usr/local/libexec/kira-service/kira-service"
printf '[Unit]\n' > "$legacy_root/usr/lib/systemd/system/kira-service.service"
"$project_root/scripts/install-nukleus.sh" --root "$legacy_root" --binary "$service_binary" \
    --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable > "$legacy_root/output"
grep -q '^Updated Nukleus\.$' "$legacy_root/output"
legacy_config="$legacy_root/etc/nukleus/nukleus.conf"
grep -qx 'NUKLEUS_LISTEN=10.0.0.93' "$legacy_config"
grep -qx 'NUKLEUS_TOKEN_FILE=/etc/nukleus/token' "$legacy_config"
grep -qx 'NUKLEUS_MODELS_DIR=/srv/lab/models' "$legacy_config"
! grep -q 'KIRA_SERVICE' "$legacy_config"
[[ "$(stat -c '%a' "$legacy_config")" == "600" ]]
grep -qx 'legacy key' "$legacy_root/etc/nukleus/token"
[[ "$(<"$legacy_root/var/lib/nukleus/data/nukleus/models/llm/chat.gguf")" == 'chat' ]]
grep -Fq -- '--models-dir ${NUKLEUS_MODELS_DIR}' "$legacy_root/usr/lib/systemd/system/nukleus.service"
[[ ! -e "$legacy_root/etc/kira-service" && ! -e "$legacy_root/var/lib/kira-service" ]]
[[ ! -e "$legacy_root/usr/local/libexec/kira-service" && ! -e "$legacy_root/usr/lib/systemd/system/kira-service.service" ]]

# Both deployments present: refuse without touching either.
mkdir -p "$legacy_root/etc/kira-service"
if "$project_root/scripts/install-nukleus.sh" --root "$legacy_root" --binary "$service_binary" \
    --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable 2> "$legacy_root/error"; then
    exit 1
fi
grep -q 'Both kira-service and nukleus deployments exist' "$legacy_root/error"
[[ -d "$legacy_root/etc/kira-service" && -f "$legacy_config" ]]
