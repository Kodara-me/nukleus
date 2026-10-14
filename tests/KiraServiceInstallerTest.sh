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

"$project_root/scripts/install-kira-service.sh" \
    --root "$stage_root" \
    --binary "$service_binary" \
    --manager-binary "$manager_binary" \
    --user-bin /home/kira-test/.local/bin \
    --listen 10.0.0.93 \
    --token "correct horse battery staple" \
    --no-enable

program="$stage_root/usr/local/libexec/kira-service/kira-service"
manager="$stage_root/home/kira-test/.local/bin/kira-service-manager"
config="$stage_root/etc/kira-service/kira-service.conf"
token="$stage_root/etc/kira-service/token"
unit="$stage_root/usr/lib/systemd/system/kira-service.service"

[[ -x "$program" ]]
[[ -x "$manager" ]]
[[ "$(stat -c '%a' "$config")" == "600" ]]
[[ "$(stat -c '%a' "$token")" == "640" ]]
[[ "$(stat -c '%a' "$unit")" == "644" ]]
grep -qx 'KIRA_SERVICE_LISTEN=10.0.0.93' "$config"
grep -qx 'KIRA_SERVICE_PORT=9384' "$config"
grep -qx 'KIRA_SERVICE_TOKEN_FILE=/etc/kira-service/token' "$config"
grep -qx 'correct horse battery staple' "$token"
grep -Fqx 'ExecStart=/usr/local/libexec/kira-service/kira-service --listen ${KIRA_SERVICE_LISTEN} --port ${KIRA_SERVICE_PORT} --token-file ${KIRA_SERVICE_TOKEN_FILE}' "$unit"
! grep -q '@KIRA_SERVICE_GPU_GROUPS@' "$unit"

replacement_binary="$stage_root/replacement-service"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$replacement_binary"
chmod +x "$replacement_binary"
mkdir -p "$stage_root/var/lib/kira-service/data/kira-canvas/models"
printf 'legacy model' > "$stage_root/var/lib/kira-service/data/kira-canvas/models/legacy.safetensors"
"$project_root/scripts/install-kira-service.sh" \
    --root "$stage_root" \
    --binary "$replacement_binary" \
    --manager-binary "$manager_binary" \
    --user-bin /home/kira-test/.local/bin \
    --listen 10.0.0.99 \
    --token "replace me" \
    --no-enable > "$stage_root/update-output"
cmp "$replacement_binary" "$program"
grep -qx 'KIRA_SERVICE_LISTEN=10.0.0.93' "$config"
grep -qx 'correct horse battery staple' "$token"
grep -q '^Updated Kira Service\.$' "$stage_root/update-output"
[[ ! -e "$stage_root/var/lib/kira-service/data/kira-canvas/models" ]]
[[ "$(<"$stage_root/var/lib/kira-service/data/kira-service/models/legacy.safetensors")" == 'legacy model' ]]

mkdir -p "$stage_root/var/lib/kira-service/data/kira-canvas/models" "$stage_root/var/lib/kira-service/data/kira-service/models"
if "$project_root/scripts/install-kira-service.sh" --root "$stage_root" --binary "$replacement_binary" --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable; then
    printf 'Expected conflicting model roots to be rejected\n' >&2
    exit 1
fi
rm -rf "$stage_root/var/lib/kira-service/data/kira-canvas"

printf '\ninteractive LAN token\n' | PATH="$mock_bin:$PATH" script -qec \
    "'$project_root/scripts/install-kira-service.sh' --root '$interactive_root' --binary '$service_binary' --manager-binary '$manager_binary' --user-bin /home/kira-test/.local/bin --no-enable" /dev/null \
    > "$interactive_root-output"
grep -qx 'KIRA_SERVICE_LISTEN=10.0.0.93' "$interactive_root/etc/kira-service/kira-service.conf"
grep -qx 'interactive LAN token' "$interactive_root/etc/kira-service/token"

printf '1\n' | PATH="$mock_bin:$PATH" script -qec \
    "'$project_root/scripts/install-kira-service.sh' --root '$loopback_root' --binary '$service_binary' --manager-binary '$manager_binary' --user-bin /home/kira-test/.local/bin --no-enable" /dev/null \
    > "$loopback_root-output"
grep -qx 'KIRA_SERVICE_LISTEN=127.0.0.1' "$loopback_root/etc/kira-service/kira-service.conf"
[[ -z "$(tr -d '\n\r' < "$loopback_root/etc/kira-service/token")" ]]

mkdir -p "$stage_root/var/lib/kira-service/data"
"$project_root/scripts/uninstall-kira-service.sh" --root "$stage_root" --user-bin /home/kira-test/.local/bin --purge --yes
[[ ! -e "$program" ]]
[[ ! -e "$manager" ]]
[[ ! -e "$config" ]]
[[ ! -e "$stage_root/var/lib/kira-service" ]]

"$project_root/scripts/install-kira-service.sh" \
    --root "$stage_root" \
    --binary "$service_binary" \
    --manager-binary "$manager_binary" \
    --user-bin /home/kira-test/.local/bin \
    --listen 10.0.0.93 \
    --token "dry run token" \
    --no-enable \
    --dry-run
[[ ! -e "$stage_root/usr/local/libexec/kira-service/kira-service" ]]

# Split storage: directories are created, configured and preserved by updates.
storage_install() {
    "$project_root/scripts/install-kira-service.sh" --root "$storage_root" --binary "$service_binary" \
        --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --listen 127.0.0.1 --no-enable "$@"
}
mkdir -p "$storage_root/home/kira-test/Bureau"
storage_install --models-dir /srv/lab/models --config-dir /srv/lab/config/kira-service \
    --desktop-dir /home/kira-test/Bureau > "$storage_root-output"
storage_config="$storage_root/etc/kira-service/kira-service.conf"

# The manager gets an application-menu entry, themed icons and a trusted desktop shortcut.
menu_entry="$storage_root/home/kira-test/.local/share/applications/kira-service-manager.desktop"
shortcut="$storage_root/home/kira-test/Bureau/kira-service-manager.desktop"
grep -Fqx 'Exec="/home/kira-test/.local/bin/kira-service-manager" --gui' "$menu_entry"
grep -qx 'Icon=/home/kira-test/.local/share/icons/hicolor/256x256/apps/kira-service-manager.png' "$menu_entry"
if grep -q '@KIRA_SERVICE_MANAGER' "$menu_entry"; then exit 1; fi
[[ "$(stat -c '%a' "$menu_entry")" == "644" ]]
cmp "$menu_entry" "$shortcut"
[[ "$(stat -c '%a' "$shortcut")" == "755" ]]
for size in 16 32 48 64 128 256 512; do
    cmp "$project_root/assets/icons/kira-service-manager-$size.png" \
        "$storage_root/home/kira-test/.local/share/icons/hicolor/${size}x${size}/apps/kira-service-manager.png"
done
grep -Fq "Desktop shortcut: $shortcut" "$storage_root-output"
if command -v desktop-file-validate >/dev/null 2>&1; then desktop-file-validate "$menu_entry"; fi
storage_unit="$storage_root/usr/lib/systemd/system/kira-service.service"
for directory in generation upscale llm companion; do [[ -d "$storage_root/srv/lab/models/$directory" ]]; done
[[ -d "$storage_root/srv/lab/config/kira-service/catalog" ]]
grep -qx 'KIRA_SERVICE_MODELS_DIR=/srv/lab/models' "$storage_config"
grep -qx 'KIRA_SERVICE_CONFIG_DIR=/srv/lab/config/kira-service' "$storage_config"
grep -Fqx 'ExecStart=/usr/local/libexec/kira-service/kira-service --listen ${KIRA_SERVICE_LISTEN} --port ${KIRA_SERVICE_PORT} --token-file ${KIRA_SERVICE_TOKEN_FILE} --models-dir ${KIRA_SERVICE_MODELS_DIR} --config-dir ${KIRA_SERVICE_CONFIG_DIR}' "$storage_unit"
grep -qx 'RequiresMountsFor=/srv/lab/models /srv/lab/config/kira-service' "$storage_unit"
grep -qx 'ReadWritePaths=/var/lib/kira-service /run/kira-service /srv/lab/models /srv/lab/config/kira-service/catalog' "$storage_unit"
grep -qx 'ProtectSystem=strict' "$storage_unit"
if grep -q '@KIRA_SERVICE_STORAGE' "$storage_unit"; then exit 1; fi

storage_install > /dev/null
grep -qx 'KIRA_SERVICE_MODELS_DIR=/srv/lab/models' "$storage_config"
grep -q -- '--models-dir ${KIRA_SERVICE_MODELS_DIR}' "$storage_unit"

storage_install --models-dir /srv/lab/other-models --config-dir /srv/lab/config/kira-service > /dev/null
grep -qx 'KIRA_SERVICE_MODELS_DIR=/srv/lab/other-models' "$storage_config"
grep -qx 'KIRA_SERVICE_LISTEN=127.0.0.1' "$storage_config"

for invalid in "--models-dir /srv/lab/models" "--models-dir relative --config-dir /srv/lab/config" "--models-dir /srv/with#hash --config-dir /srv/lab/config"; do
    # shellcheck disable=SC2086
    if storage_install $invalid > /dev/null 2>&1; then
        printf 'Expected invalid storage options to be rejected: %s\n' "$invalid" >&2
        exit 1
    fi
done

# The legacy unit renders no storage options.
legacy_unit="$loopback_root/usr/lib/systemd/system/kira-service.service"
if grep -q -e 'RequiresMountsFor' -e '--models-dir' -e '@KIRA_SERVICE_STORAGE' "$legacy_unit"; then exit 1; fi
grep -qx 'ReadWritePaths=/var/lib/kira-service /run/kira-service' "$legacy_unit"

# The desktop shortcut is opt-in; uninstall removes the desktop files.
rm "$shortcut"
storage_install > "$storage_root-output"
if grep -q 'Desktop shortcut:' "$storage_root-output" || [[ -e "$shortcut" ]]; then exit 1; fi
[[ -f "$menu_entry" ]]
storage_install --desktop-dir /home/kira-test/Bureau > /dev/null
[[ -f "$shortcut" ]]
printf 'unrelated' > "$storage_root/home/kira-test/Bureau/other.desktop"
"$project_root/scripts/uninstall-kira-service.sh" --root "$storage_root" --user-bin /home/kira-test/.local/bin \
    --desktop-dir /home/kira-test/Bureau > /dev/null
[[ ! -e "$menu_entry" && ! -e "$shortcut" ]]
[[ ! -e "$storage_root/home/kira-test/.local/share/icons/hicolor/48x48/apps/kira-service-manager.png" ]]
[[ -f "$storage_root/home/kira-test/Bureau/other.desktop" && -d "$storage_root/srv/lab/models/generation" ]]
