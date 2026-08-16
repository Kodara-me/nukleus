#!/usr/bin/env bash

set -euo pipefail

project_root="$1"
service_binary="$2"
manager_binary="$3"
stage_root="$(mktemp -d)"
mock_bin="$(mktemp -d)"
interactive_root="$(mktemp -d)"
loopback_root="$(mktemp -d)"
trap 'rm -rf "$stage_root" "$mock_bin" "$interactive_root" "$loopback_root"' EXIT

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
