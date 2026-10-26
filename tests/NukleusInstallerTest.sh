#!/usr/bin/env bash

set -euo pipefail

project_root="$1"
service_binary="$2"
manager_binary="$3"
web_binary="$4"
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

"$project_root/scripts/install-nukleus.sh" --install-backend \
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
"$project_root/scripts/install-nukleus.sh" --install-backend \
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
if "$project_root/scripts/install-nukleus.sh" --install-backend --root "$stage_root" --binary "$replacement_binary" --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable; then
    printf 'Expected conflicting model roots to be rejected\n' >&2
    exit 1
fi
rm -rf "$stage_root/var/lib/nukleus/data/kira-canvas"

printf '\ninteractive LAN token\n' | PATH="$mock_bin:$PATH" script -qec \
    "'$project_root/scripts/install-nukleus.sh' --install-backend --root '$interactive_root' --binary '$service_binary' --manager-binary '$manager_binary' --user-bin /home/kira-test/.local/bin --no-enable" /dev/null \
    > "$interactive_root-output"
grep -qx 'NUKLEUS_LISTEN=10.0.0.93' "$interactive_root/etc/nukleus/nukleus.conf"
grep -qx 'interactive LAN token' "$interactive_root/etc/nukleus/token"

printf '1\n' | PATH="$mock_bin:$PATH" script -qec \
    "'$project_root/scripts/install-nukleus.sh' --install-backend --root '$loopback_root' --binary '$service_binary' --manager-binary '$manager_binary' --user-bin /home/kira-test/.local/bin --no-enable" /dev/null \
    > "$loopback_root-output"
grep -qx 'NUKLEUS_LISTEN=127.0.0.1' "$loopback_root/etc/nukleus/nukleus.conf"
[[ -z "$(tr -d '\n\r' < "$loopback_root/etc/nukleus/token")" ]]

mkdir -p "$stage_root/var/lib/nukleus/data"
"$project_root/scripts/uninstall-nukleus.sh" --root "$stage_root" --user-bin /home/kira-test/.local/bin --purge --yes
[[ ! -e "$program" ]]
[[ ! -e "$manager" ]]
[[ ! -e "$config" ]]
[[ ! -e "$stage_root/var/lib/nukleus" ]]

"$project_root/scripts/install-nukleus.sh" --install-backend \
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
    "$project_root/scripts/install-nukleus.sh" --install-backend --root "$storage_root" --binary "$service_binary" \
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
legacy_home="$legacy_root/home/kira-test"
mkdir -p "$legacy_home/.local/bin" "$legacy_home/.local/share/applications" \
    "$legacy_home/.local/share/icons/hicolor/48x48/apps" "$legacy_home/Bureau"
printf 'old' > "$legacy_home/.local/bin/kira-service-manager"
printf 'old' > "$legacy_home/.local/share/applications/kira-service-manager.desktop"
printf 'old' > "$legacy_home/.local/share/icons/hicolor/48x48/apps/kira-service-manager.png"
printf 'StartupWMClass=kira-service-manager\n' > "$legacy_home/Bureau/kira-service-manager.desktop"
printf 'unrelated\n' > "$legacy_home/Bureau/kira-service-manager-notes.desktop"
# A dry run reads the legacy configuration in place, asks nothing and moves nothing.
"$project_root/scripts/install-nukleus.sh" --install-backend --root "$legacy_root" --binary "$service_binary" \
    --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable --dry-run \
    < /dev/null > "$legacy_root/dry-run"
grep -q '^+ rewrite .*kira-service.conf' "$legacy_root/dry-run"
grep -q '^Updated Nukleus\.$' "$legacy_root/dry-run"
grep -q 'listens on 127.0.0.1:9384 and 10.0.0.93:9384' "$legacy_root/dry-run"
[[ -f "$legacy_root/etc/kira-service/kira-service.conf" && ! -e "$legacy_root/etc/nukleus" ]]
[[ -f "$legacy_home/.local/bin/kira-service-manager" ]]
"$project_root/scripts/install-nukleus.sh" --install-backend --root "$legacy_root" --binary "$service_binary" \
    --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable \
    --desktop-dir /home/kira-test/Bureau > "$legacy_root/output"
[[ ! -e "$legacy_home/.local/bin/kira-service-manager" ]]
[[ ! -e "$legacy_home/.local/share/applications/kira-service-manager.desktop" ]]
[[ ! -e "$legacy_home/.local/share/icons/hicolor/48x48/apps/kira-service-manager.png" ]]
[[ ! -e "$legacy_home/Bureau/kira-service-manager.desktop" && -f "$legacy_home/Bureau/kira-service-manager-notes.desktop" ]]
[[ -x "$legacy_home/.local/bin/nukleus-manager" ]]
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
if "$project_root/scripts/install-nukleus.sh" --install-backend --root "$legacy_root" --binary "$service_binary" \
    --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --no-enable 2> "$legacy_root/error"; then
    exit 1
fi
grep -q 'Both kira-service and nukleus deployments exist' "$legacy_root/error"
[[ -d "$legacy_root/etc/kira-service" && -f "$legacy_config" ]]

# Storage groups are resolved by id: a group id with no name stops the
# installer before any change. The test seam derives storage groups from the
# staged trees, and a mocked getent hides the name of the test user's group.
group_tests="$(mktemp -d)"
trap 'rm -rf "$stage_root" "$mock_bin" "$interactive_root" "$loopback_root" "$storage_root" "$storage_root-output" "$legacy_root" "$group_tests"' EXIT
real_getent="$(command -v getent)"
nameless_bin="$group_tests/bin"
mkdir -p "$nameless_bin"
probe="$group_tests/probe"
mkdir "$probe"
test_gid="$(stat -c '%g' "$probe")"
test_group="$("$real_getent" group "$test_gid" | cut -d: -f1)"
[[ -n "$test_group" && "$test_group" != root ]]
printf '%s\n' '#!/usr/bin/env bash' \
    "if [[ \"\${1:-}\" == group && \"\${2:-}\" == $test_gid ]]; then exit 2; fi" \
    "exec '$real_getent' \"\$@\"" > "$nameless_bin/getent"
chmod +x "$nameless_bin/getent"

seam_install() {
    local root="$1"
    shift
    NUKLEUS_INSTALLER_TEST_STORAGE_GROUPS=1 "$project_root/scripts/install-nukleus.sh" --root "$root" \
        --binary "$service_binary" --manager-binary "$manager_binary" \
        --user-bin /home/kira-test/.local/bin --listen 127.0.0.1 --no-enable "$@"
}
nameless_install() {
    PATH="$nameless_bin:$PATH" seam_install "$@"
}
tree_snapshot() {
    (cd "$1" && find . -printf '%p %y %m %g %s %T@\n' | LC_ALL=C sort)
}
# R1, R3, R4: one message names every affected directory, the id and both fixes.
expect_nameless_failure() {
    local root="$1" errors="$2" output="$3"
    grep -Fq "$root/srv/lab/models (group id $test_gid)" "$errors"
    grep -Fq "$root/srv/lab/config/nukleus/catalog (group id $test_gid)" "$errors"
    grep -Fq "find $root/srv/lab/models -gid $test_gid -exec chgrp -h <group> {} +" "$errors"
    grep -Fq "find $root/srv/lab/config/nukleus/catalog -gid $test_gid -exec chgrp -h <group> {} +" "$errors"
    grep -Fq "groupadd -g $test_gid <name>" "$errors"
    if grep -q 'UNKNOWN' "$errors" "$output"; then exit 1; fi
    # R2: no action was printed before the failure.
    if grep -q '^+ ' "$output"; then exit 1; fi
}

# R1-R4, R2: paths from the command line, real run.
cli_root="$group_tests/cli"
mkdir -p "$cli_root/srv/lab/models" "$cli_root/srv/lab/config/nukleus/catalog"
before="$(tree_snapshot "$cli_root")"
set +e
nameless_install "$cli_root" --install-backend --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus \
    > "$group_tests/cli-output" 2> "$group_tests/cli-errors"
real_status=$?
set -e
(( real_status != 0 ))
expect_nameless_failure "$cli_root" "$group_tests/cli-errors" "$group_tests/cli-output"
[[ "$(tree_snapshot "$cli_root")" == "$before" && ! -e "$cli_root/etc/nukleus" ]]

# R6: the dry run reports the same message with the same status.
set +e
nameless_install "$cli_root" --install-backend --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus --dry-run \
    > "$group_tests/dry-output" 2> "$group_tests/dry-errors"
dry_status=$?
set -e
(( dry_status == real_status ))
cmp "$group_tests/cli-errors" "$group_tests/dry-errors"
expect_nameless_failure "$cli_root" "$group_tests/dry-errors" "$group_tests/dry-output"
[[ "$(tree_snapshot "$cli_root")" == "$before" ]]

# R5, R2: paths kept from an existing nukleus.conf during an update.
update_root="$group_tests/update"
mkdir -p "$update_root/etc/nukleus" "$update_root/srv/lab/models" "$update_root/srv/lab/config/nukleus/catalog"
printf 'NUKLEUS_LISTEN=127.0.0.1\nNUKLEUS_PORT=9384\nNUKLEUS_TOKEN_FILE=/etc/nukleus/token\nNUKLEUS_MODELS_DIR=/srv/lab/models\nNUKLEUS_CONFIG_DIR=/srv/lab/config/nukleus\n' \
    > "$update_root/etc/nukleus/nukleus.conf"
printf '\n' > "$update_root/etc/nukleus/token"
mkdir -p "$update_root/usr/lib/systemd/system"
printf '[Unit]\n' > "$update_root/usr/lib/systemd/system/nukleus.service"
before="$(tree_snapshot "$update_root")"
if nameless_install "$update_root" > "$group_tests/update-output" 2> "$group_tests/update-errors"; then exit 1; fi
expect_nameless_failure "$update_root" "$group_tests/update-errors" "$group_tests/update-output"
[[ "$(tree_snapshot "$update_root")" == "$before" ]]

# R5, R2: paths read in place from a legacy kira-service.conf; nothing is migrated.
legacy_group_root="$group_tests/legacy"
mkdir -p "$legacy_group_root/etc/kira-service" "$legacy_group_root/var/lib/kira-service/data" \
    "$legacy_group_root/usr/local/libexec/kira-service" "$legacy_group_root/usr/lib/systemd/system" \
    "$legacy_group_root/srv/lab/models" "$legacy_group_root/srv/lab/config/nukleus/catalog"
printf 'KIRA_SERVICE_LISTEN=127.0.0.1\nKIRA_SERVICE_PORT=9384\nKIRA_SERVICE_TOKEN_FILE=/etc/kira-service/token\nKIRA_SERVICE_MODELS_DIR=/srv/lab/models\nKIRA_SERVICE_CONFIG_DIR=/srv/lab/config/nukleus\n' \
    > "$legacy_group_root/etc/kira-service/kira-service.conf"
printf 'old' > "$legacy_group_root/usr/local/libexec/kira-service/kira-service"
printf '[Unit]\n' > "$legacy_group_root/usr/lib/systemd/system/kira-service.service"
before="$(tree_snapshot "$legacy_group_root")"
if nameless_install "$legacy_group_root" > "$group_tests/legacy-output" 2> "$group_tests/legacy-errors"; then exit 1; fi
expect_nameless_failure "$legacy_group_root" "$group_tests/legacy-errors" "$group_tests/legacy-output"
[[ -f "$legacy_group_root/etc/kira-service/kira-service.conf" && -d "$legacy_group_root/var/lib/kira-service" ]]
[[ -f "$legacy_group_root/usr/lib/systemd/system/kira-service.service" ]]
[[ -f "$legacy_group_root/usr/local/libexec/kira-service/kira-service" ]]
[[ ! -e "$legacy_group_root/etc/nukleus" ]]
[[ "$(tree_snapshot "$legacy_group_root")" == "$before" ]]

# R7, R8: a named shared group is joined by name, never by id or placeholder.
named_root="$group_tests/named"
mkdir -p "$named_root/srv/lab/models" "$named_root/srv/lab/config/nukleus"
seam_install "$named_root" --install-backend --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus --dry-run \
    > "$group_tests/named-dry-output"
grep -Fqx "+ install -d -g $test_group -m 2775 $named_root/srv/lab/models $named_root/srv/lab/models/generation $named_root/srv/lab/models/upscale $named_root/srv/lab/models/llm $named_root/srv/lab/models/companion " \
    "$group_tests/named-dry-output"
grep -Fqx "+ install -d -g $test_group -m 2775 $named_root/srv/lab/config/nukleus/catalog " "$group_tests/named-dry-output"
seam_install "$named_root" --install-backend --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus \
    > "$group_tests/named-output"
grep -Fqx "+ usermod --append --groups $test_group nukleus " "$group_tests/named-output"
[[ "$(grep -c '^+ usermod' "$group_tests/named-output")" == 1 ]]
grep -qx "SupplementaryGroups=$test_group" "$named_root/usr/lib/systemd/system/nukleus.service"
for directory in "" /generation /upscale /llm /companion; do
    [[ "$(stat -c '%a %G' "$named_root/srv/lab/models$directory")" == "2775 $test_group" ]]
done
[[ "$(stat -c '%a %G' "$named_root/srv/lab/config/nukleus/catalog")" == "2775 $test_group" ]]
if grep -Eq -e 'UNKNOWN' -e "(-g|--groups|SupplementaryGroups=) ?$test_gid( |$)" \
    "$group_tests/named-dry-output" "$group_tests/named-output" "$named_root/usr/lib/systemd/system/nukleus.service"; then
    exit 1
fi

# Components: the backend and the web service are chosen by flags (R19, R20, R37, R38).
component_tests="$(mktemp -d)"
trap 'rm -rf "$stage_root" "$mock_bin" "$interactive_root" "$loopback_root" "$storage_root" "$storage_root-output" "$legacy_root" "$group_tests" "$component_tests"' EXIT
replacement_web="$component_tests/replacement-web"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$replacement_web"
chmod +x "$replacement_web"
absent="$component_tests/absent"

# Runs the installer on a root, with no account of the machine involved: the manager user
# is never resolvable here, so a run that needs it would fail.
component_install() {
    local root="$1"
    shift
    env -u USER -u SUDO_USER -u HOME "$project_root/scripts/install-nukleus.sh" --root "$root" --no-enable "$@" < /dev/null
}
backend_args=(--binary "$service_binary" --manager-binary "$manager_binary" --user-bin /home/kira-test/.local/bin --listen 127.0.0.1)
web_args=(--web-binary "$web_binary" --web-listen 127.0.0.1)
web_unit_rel=usr/lib/systemd/system/nukleus-web.service
web_program_rel=usr/local/libexec/nukleus/nukleus-web
backend_unit_rel=usr/lib/systemd/system/nukleus.service

# R19, R38: the backend alone needs no nukleus-web executable and installs no web service.
root="$component_tests/backend-only"
component_install "$root" --install-backend "${backend_args[@]}" --web-binary "$absent" > /dev/null
[[ -x "$root/usr/local/libexec/nukleus/nukleusd" && -f "$root/$backend_unit_rel" ]]
[[ ! -e "$root/$web_unit_rel" && ! -e "$root/$web_program_rel" && ! -e "$root/etc/nukleus/nukleus-web.conf" ]]

# R19, R38: the web service alone needs no nukleusd, manager executable or manager user.
root="$component_tests/web-only"
component_install "$root" --install-web-service "${web_args[@]}" --web-backend-host 10.0.0.50 \
    --web-backend-token "remote token" --binary "$absent" --manager-binary "$absent" > /dev/null
[[ -x "$root/$web_program_rel" && -f "$root/$web_unit_rel" ]]
[[ ! -e "$root/$backend_unit_rel" && ! -e "$root/usr/local/libexec/nukleus/nukleusd" && ! -e "$root/home" ]]

# R19: both flags install both components.
root="$component_tests/both"
component_install "$root" --install-backend --install-web-service "${backend_args[@]}" "${web_args[@]}" > /dev/null
[[ -f "$root/$backend_unit_rel" && -f "$root/$web_unit_rel" && -x "$root/$web_program_rel" ]]

# R37: no component and no flag stops before any change, naming both flags.
root="$component_tests/empty"
mkdir "$root"
if component_install "$root" "${backend_args[@]}" "${web_args[@]}" > "$root-output" 2> "$root-errors"; then exit 1; fi
grep -Fq -- '--install-backend' "$root-errors"
grep -Fq -- '--install-web-service' "$root-errors"
[[ -z "$(find "$root" -mindepth 1)" ]]
if component_install "$root" "${backend_args[@]}" "${web_args[@]}" --dry-run > "$root-output" 2> "$root-errors"; then exit 1; fi
grep -Fq -- '--install-web-service' "$root-errors"
if grep -q '^+ ' "$root-output"; then exit 1; fi

# R20: an installed component is updated with no flag, and none is added.
root="$component_tests/both"
component_install "$root" "${backend_args[@]}" --web-binary "$replacement_web" --binary "$replacement_web" > "$root-output"
cmp "$replacement_web" "$root/$web_program_rel"
cmp "$replacement_web" "$root/usr/local/libexec/nukleus/nukleusd"
root="$component_tests/backend-only"
component_install "$root" "${backend_args[@]}" "${web_args[@]}" --binary "$replacement_web" > "$root-output"
cmp "$replacement_web" "$root/usr/local/libexec/nukleus/nukleusd"
grep -q '^Updated Nukleus\.$' "$root-output"
[[ ! -e "$root/$web_unit_rel" && ! -e "$root/$web_program_rel" ]]
root="$component_tests/web-only"
component_install "$root" "${web_args[@]}" --web-binary "$replacement_web" --binary "$absent" --manager-binary "$absent" > "$root-output"
cmp "$replacement_web" "$root/$web_program_rel"
grep -q '^Updated the Nukleus web service\.$' "$root-output"
[[ ! -e "$root/$backend_unit_rel" && ! -e "$root/usr/local/libexec/nukleus/nukleusd" ]]

# R20: a flag adds its component and the installed one is updated too.
root="$component_tests/backend-only"
component_install "$root" --install-web-service "${backend_args[@]}" "${web_args[@]}" --binary "$replacement_web" > /dev/null
[[ -f "$root/$web_unit_rel" && -x "$root/$web_program_rel" ]]
cmp "$replacement_web" "$root/usr/local/libexec/nukleus/nukleusd"
root="$component_tests/web-only"
printf 'stale' > "$root/$web_program_rel"
component_install "$root" --install-backend "${backend_args[@]}" "${web_args[@]}" > /dev/null
[[ -f "$root/$backend_unit_rel" ]]
cmp "$web_binary" "$root/$web_program_rel"

# R21: the program, the unit and the account.
unit="$component_tests/both/$web_unit_rel"
grep -qx 'User=nukleus-web' "$unit"
grep -qx 'ExecStart=/usr/local/libexec/nukleus/nukleus-web' "$unit"
grep -qx 'EnvironmentFile=/etc/nukleus/nukleus-web.conf' "$unit"
[[ "$(stat -c '%a' "$unit")" == "644" ]]
root="$component_tests/accounts"
NUKLEUS_INSTALLER_TEST_ACCOUNTS=1 component_install "$root" --install-web-service "${web_args[@]}" \
    --web-backend-host 10.0.0.50 --web-backend-token "remote token" > "$root-output"
grep -Fqx '+ groupadd --system nukleus-web ' "$root-output"
grep -Eq '^\+ useradd --system --gid nukleus-web .* nukleus-web $' "$root-output"
# Without the seam, a staged root creates no account and runs no chown.
root="$component_tests/no-seam"
component_install "$root" --install-web-service "${web_args[@]}" \
    --web-backend-host 10.0.0.50 --web-backend-token "remote token" > "$root-output"
if grep -Eq 'groupadd|useradd|chown' "$root-output"; then exit 1; fi
# An account already in the staged root is not created again.
root="$component_tests/accounts-present"
mkdir -p "$root/etc"
printf 'nukleus-web:x:900:\n' > "$root/etc/group"
printf 'nukleus-web:x:900:900::/nonexistent:/usr/sbin/nologin\n' > "$root/etc/passwd"
NUKLEUS_INSTALLER_TEST_ACCOUNTS=1 component_install "$root" --install-web-service "${web_args[@]}" \
    --web-backend-host 10.0.0.50 --web-backend-token "remote token" > "$root-output"
if grep -Eq 'groupadd|useradd' "$root-output"; then exit 1; fi

# R40: the listen address comes from --web-listen.
root="$component_tests/listen"
component_install "$root" --install-web-service --web-binary "$web_binary" --web-listen 10.0.0.93 \
    --web-backend-host 10.0.0.50 --web-backend-token "remote token" > /dev/null
grep -qx 'NUKLEUS_WEB_LISTEN=10.0.0.93' "$root/etc/nukleus/nukleus-web.conf"

# R48: without the option and without a terminal, the installer stops before any change.
root="$component_tests/no-listen"
mkdir "$root"
if component_install "$root" --install-web-service --web-binary "$web_binary" \
    --web-backend-host 10.0.0.50 --web-backend-token "remote token" > "$root-output" 2> "$root-errors"; then exit 1; fi
grep -Fq -- '--web-listen' "$root-errors"
[[ -z "$(find "$root" -mindepth 1)" ]]

# R22: with the backend installed by the same run or already on the host, the web service
# uses loopback and no host or token is asked.
for root in "$component_tests/both" "$component_tests/backend-only"; do
    conf="$root/etc/nukleus/nukleus-web.conf"
    grep -qx 'NUKLEUS_WEB_BACKEND_HOST=127.0.0.1' "$conf"
    if grep -q 'TOKEN_FILE' "$conf" || [[ -e "$root/etc/nukleus/web-backend-token" ]]; then exit 1; fi
done

# R41, R42: another host's backend: host and token from the options, token file 0640.
root="$component_tests/accounts"
conf="$root/etc/nukleus/nukleus-web.conf"
grep -qx 'NUKLEUS_WEB_BACKEND_HOST=10.0.0.50' "$conf"
grep -qx 'NUKLEUS_WEB_BACKEND_TOKEN_FILE=/etc/nukleus/web-backend-token' "$conf"
grep -qx 'remote token' "$root/etc/nukleus/web-backend-token"
[[ "$(stat -c '%a' "$root/etc/nukleus/web-backend-token")" == "640" ]]
[[ "$(stat -c '%a' "$conf")" == "600" ]]
root="$component_tests/accounts-chown"
NUKLEUS_INSTALLER_TEST_ACCOUNTS=1 component_install "$root" --install-web-service "${web_args[@]}" \
    --web-backend-host 10.0.0.50 --web-backend-token "remote token" > "$root-output"
grep -Fqx "+ chown root:nukleus-web $root/etc/nukleus/web-backend-token " "$root-output"

# R41: the host and the token are asked, the token masked, when input is interactive.
root="$component_tests/interactive-web"
printf '1\n10.0.0.77\ninteractive remote token\n' | PATH="$mock_bin:$PATH" script -qec \
    "env -u USER -u SUDO_USER -u HOME '$project_root/scripts/install-nukleus.sh' --root '$root' --install-web-service --web-binary '$web_binary' --no-enable" /dev/null \
    > "$root-output"
grep -qx 'NUKLEUS_WEB_LISTEN=127.0.0.1' "$root/etc/nukleus/nukleus-web.conf"
grep -qx 'NUKLEUS_WEB_BACKEND_HOST=10.0.0.77' "$root/etc/nukleus/nukleus-web.conf"
grep -qx 'interactive remote token' "$root/etc/nukleus/web-backend-token"
grep -q 'Nukleus LAN token: ' "$root-output"

# R43: without the host or the token and without a terminal, the installer stops before any change.
for missing in "" "--web-backend-host 10.0.0.50" "--web-backend-token abc"; do
    root="$component_tests/no-host"
    rm -rf "$root"
    mkdir "$root"
    # shellcheck disable=SC2086
    if component_install "$root" --install-web-service "${web_args[@]}" $missing > "$root-output" 2> "$root-errors"; then exit 1; fi
    grep -Fq -- '--web-backend-host' "$root-errors"
    grep -Fq -- '--web-backend-token' "$root-errors"
    [[ -z "$(find "$root" -mindepth 1)" ]]
done

# R39: a later run with no web option keeps the configuration and the token file.
root="$component_tests/listen"
before="$(cat "$root/etc/nukleus/nukleus-web.conf" "$root/etc/nukleus/web-backend-token")"
component_install "$root" --web-binary "$web_binary" > "$root-output"
[[ "$(cat "$root/etc/nukleus/nukleus-web.conf" "$root/etc/nukleus/web-backend-token")" == "$before" ]]
grep -q '^Updated the Nukleus web service\.$' "$root-output"
component_install "$root" --install-web-service --web-binary "$web_binary" --web-listen 10.0.0.1 \
    --web-backend-host 10.0.0.2 --web-backend-token other > /dev/null
[[ "$(cat "$root/etc/nukleus/nukleus-web.conf" "$root/etc/nukleus/web-backend-token")" == "$before" ]]

# A dry run changes nothing and prints its actions.
root="$component_tests/dry"
mkdir "$root"
component_install "$root" --install-web-service "${web_args[@]}" --web-backend-host 10.0.0.50 \
    --web-backend-token "dry token" --dry-run > "$root-output"
[[ -z "$(find "$root" -mindepth 1)" ]]
grep -q '^+ write .*nukleus-web.conf' "$root-output"

# R23: the uninstaller removes the web service and keeps its configuration without --purge.
root="$component_tests/accounts"
mkdir -p "$root/var/lib/nukleus"
for purge in "" "--purge --yes"; do
    # shellcheck disable=SC2086
    "$project_root/scripts/uninstall-nukleus.sh" --root "$root" --user-bin /home/kira-test/.local/bin $purge --dry-run > "$root-output"
    if grep -Eq 'userdel|groupdel' "$root-output"; then exit 1; fi
done
[[ -f "$root/$web_unit_rel" ]]
"$project_root/scripts/uninstall-nukleus.sh" --root "$root" --user-bin /home/kira-test/.local/bin > /dev/null
[[ ! -e "$root/$web_unit_rel" && ! -e "$root/$web_program_rel" ]]
[[ -f "$root/etc/nukleus/nukleus-web.conf" && -f "$root/etc/nukleus/web-backend-token" ]]
root="$component_tests/both"
"$project_root/scripts/uninstall-nukleus.sh" --root "$root" --user-bin /home/kira-test/.local/bin --purge --yes > /dev/null
[[ ! -e "$root/$web_unit_rel" && ! -e "$root/$web_program_rel" && ! -e "$root/etc/nukleus" ]]
