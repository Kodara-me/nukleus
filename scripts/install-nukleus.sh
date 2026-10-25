#!/usr/bin/env bash

set -euo pipefail

readonly SERVICE_NAME="nukleus"
readonly SERVICE_USER="nukleus"
readonly SERVICE_GROUP="nukleus"
readonly DEFAULT_PORT=9384

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
binary_path="$project_root/build/src/nukleusd"
manager_binary_path="$project_root/build/src/nukleus-manager"
listen_address=""
token=""
install_root=""
user_bin_path=""
manager_user=""
dry_run=false
enable_service=true
models_dir=""
storage_config_dir=""
desktop_dir=""
desktop_shortcut=false

usage() {
    cat <<'EOF'
Usage: sudo scripts/install-nukleus.sh [options]

Install or update the Nukleus systemd unit and local model manager.
Fresh interactive installs offer loopback-only binding and detected LAN IPv4
addresses. A LAN address also keeps the service available on loopback.

Options:
  --binary PATH       Built nukleusd executable (default: build/src/nukleusd)
  --manager-binary PATH
                      Built nukleus-manager executable (default: build/src/nukleus-manager)
  --user-bin PATH     User-local bin directory for nukleus-manager
  --listen ADDRESS    LAN bind address, or 127.0.0.1 for local-only service
  --token TOKEN       Shared LAN passphrase; avoid this option in a shared shell history
  --models-dir DIR    Shared model weight directory for the split storage layout;
                      requires --config-dir and is preserved by later updates
  --config-dir DIR    Configuration directory; the catalogue lives in DIR/catalog
  --desktop-shortcut  Also put a manager shortcut on the user's XDG desktop
  --desktop-dir PATH  Put the manager shortcut in PATH (implies --desktop-shortcut)
  --root PATH         Stage files under PATH instead of the live filesystem
  --dry-run           Print actions without modifying the filesystem
  --no-enable         Install without enabling or starting the systemd service
  -h, --help          Show this help
EOF
}

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

print_action() {
    printf '+ '
    printf '%q ' "$@"
    printf '\n'
}

run() {
    if "$dry_run"; then
        print_action "$@"
        return
    fi
    "$@"
}

destination() { printf '%s%s' "$install_root" "$1"; }

valid_port() { [[ "$1" =~ ^[0-9]+$ ]] && (( $1 >= 1 && $1 <= 65535 )); }
valid_address() { [[ "$1" =~ ^[0-9A-Fa-f:.]+$ ]]; }
is_any_address() { [[ "$1" == "0.0.0.0" || "$1" == "::" ]]; }
is_loopback_address() { [[ "$1" == "127.0.0.1" || "$1" == "::1" ]]; }
token_has_content() { [[ -n "$(tr -d '[:space:]' <<<"$1")" ]]; }
# Storage paths are rendered into the systemd unit and a sed replacement.
valid_storage_path() { [[ "$1" =~ ^/[A-Za-z0-9._/+-]*$ ]]; }

# The directory whose group decides the storage group: the path itself, or
# its nearest existing parent when it does not exist yet.
storage_inspected_path() {
    local path="$1"
    while [[ ! -e "$path" ]]; do path="$(dirname -- "$path")"; done
    printf '%s' "$path"
}

# Prints the name of the group that owns a directory, resolved from its
# numeric id. Fails when that id has no name on the host, so that no
# placeholder such as UNKNOWN or a bare id is ever used as a group name.
storage_group_name() {
    local gid entry
    gid="$(stat -c '%g' -- "$1")"
    entry="$(getent group "$gid")" || return 1
    [[ -n "${entry%%:*}" ]] || return 1
    printf '%s' "${entry%%:*}"
}

# A storage tree whose group is not root is shared: the service joins that
# group. Otherwise the service owns its storage directories privately.
storage_group_for() {
    local path group
    path="$(storage_inspected_path "$1")"
    group="$(storage_group_name "$path")" \
        || fail "The group of $path has no name on this host."
    [[ "$group" == root ]] && printf '%s' "$SERVICE_GROUP" || printf '%s' "$group"
}

# Stops the installer, before any change to the host, when a storage directory
# belongs to a group id that has no name. Every such directory is reported.
check_storage_groups() {
    local directory path gid seen
    local -a inspected=() orphans=() orphan_ids=()
    for directory in "$@"; do
        path="$(storage_inspected_path "$directory")"
        for seen in "${inspected[@]}"; do [[ "$seen" != "$path" ]] || continue 2; done
        inspected+=("$path")
        storage_group_name "$path" >/dev/null && continue
        gid="$(stat -c '%g' -- "$path")"
        orphans+=("$path|$gid")
        [[ " ${orphan_ids[*]} " == *" $gid "* ]] || orphan_ids+=("$gid")
    done
    (( ${#orphans[@]} > 0 )) || return 0
    local message="Storage directories belong to a group id that has no name on this host:"
    local orphan
    for orphan in "${orphans[@]}"; do
        message+=$'\n'"  ${orphan%|*} (group id ${orphan##*|})"
    done
    message+=$'\n'"Give each tree a named group, for example:"
    for orphan in "${orphans[@]}"; do
        message+=$'\n'"  find ${orphan%|*} -gid ${orphan##*|} -exec chgrp -h <group> {} +"
    done
    message+=$'\n'"or create a group with that id:"
    for gid in "${orphan_ids[@]}"; do
        message+=$'\n'"  groupadd -g $gid <name>"
    done
    message+=$'\n'"Then run the installer again."
    fail "$message"
}

config_value() {
    local name="$1"
    local file="$2"
    sed -n "s/^${name}=//p" "$file" | head -n 1
}

detected_ipv4_addresses() {
    command -v ip >/dev/null 2>&1 || return
    ip -o -4 addr show scope global 2>/dev/null | awk '{ split($4, address, "/"); if (address[1] !~ /^127\./) print $2 "|" address[1] }'
}

choose_listen_address() {
    local -a addresses=()
    local entry interface address selection default_selection index
    while IFS= read -r entry; do
        [[ -n "$entry" ]] && addresses+=("$entry")
    done < <(detected_ipv4_addresses)
    [[ -t 0 ]] || fail "Provide --listen ADDRESS when input is not interactive."

    printf 'Choose where Nukleus listens:\n'
    printf '  1) Local computer only (127.0.0.1)\n'
    index=2
    for entry in "${addresses[@]}"; do
        interface="${entry%%|*}"
        address="${entry#*|}"
        printf '  %d) %s (%s)\n' "$index" "$address" "$interface"
        index=$((index + 1))
    done

    default_selection=1
    if (( ${#addresses[@]} > 0 )); then default_selection=2; fi
    read -r -p "Binding [$default_selection]: " selection
    selection="${selection:-$default_selection}"
    [[ "$selection" =~ ^[0-9]+$ ]] || fail "Choose one of the listed bindings."
    if (( selection == 1 )); then
        listen_address="127.0.0.1"
        return
    fi
    (( selection >= 2 && selection <= ${#addresses[@]} + 1 )) || fail "Choose one of the listed bindings."
    listen_address="${addresses[$((selection - 2))]#*|}"
}

while (( $# > 0 )); do
    case "$1" in
    --binary) binary_path="${2:-}"; shift 2 ;;
    --manager-binary) manager_binary_path="${2:-}"; shift 2 ;;
    --user-bin) user_bin_path="${2:-}"; shift 2 ;;
    --listen) listen_address="${2:-}"; shift 2 ;;
    --token) token="${2:-}"; shift 2 ;;
    --models-dir) models_dir="${2:-}"; shift 2 ;;
    --config-dir) storage_config_dir="${2:-}"; shift 2 ;;
    --desktop-dir) desktop_dir="${2:-}"; desktop_shortcut=true; shift 2 ;;
    --desktop-shortcut) desktop_shortcut=true; shift ;;
    --root) install_root="${2:-}"; shift 2 ;;
    --dry-run) dry_run=true; shift ;;
    --no-enable) enable_service=false; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "Unknown option: $1" ;;
    esac
done

[[ -n "$install_root" || $EUID -eq 0 ]] || fail "Run with sudo, or use --root for a staging installation."
[[ -x "$binary_path" ]] || fail "Nukleus executable is not executable: $binary_path"
[[ -x "$manager_binary_path" ]] || fail "Nukleus Manager executable is not executable: $manager_binary_path"
[[ -f "$project_root/packaging/systemd/nukleus.service.in" ]] || fail "Systemd unit template is missing."
[[ -f "$project_root/packaging/desktop/nukleus-manager.desktop.in" ]] || fail "Desktop entry template is missing."
[[ -z "$desktop_dir" || "$desktop_dir" == /* ]] || fail "--desktop-dir must be an absolute path."
if [[ -n "$models_dir" || -n "$storage_config_dir" ]]; then
    [[ -n "$models_dir" && -n "$storage_config_dir" ]] || fail "--models-dir and --config-dir must be given together."
    valid_storage_path "$models_dir" || fail "--models-dir must be an absolute path of letters, digits and ._/+-."
    valid_storage_path "$storage_config_dir" || fail "--config-dir must be an absolute path of letters, digits and ._/+-."
    models_dir="$(realpath -m -- "$models_dir")"
    storage_config_dir="$(realpath -m -- "$storage_config_dir")"
fi

if [[ -z "$user_bin_path" ]]; then
    if [[ "$EUID" -eq 0 ]]; then
        [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]] || fail "Provide --user-bin when running as root without SUDO_USER."
        manager_user="$SUDO_USER"
        user_home="$(getent passwd "$manager_user" | cut -d: -f6)"
        [[ -n "$user_home" ]] || fail "Could not resolve the home directory for $manager_user."
        user_bin_path="$user_home/.local/bin"
    else
        manager_user="${USER:-}"
        [[ -n "$manager_user" ]] || fail "Could not resolve the current user."
        user_bin_path="${HOME:?}/.local/bin"
    fi
else
    [[ "$user_bin_path" == /* ]] || fail "--user-bin must be an absolute path."
    if [[ "$EUID" -eq 0 ]]; then
        [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]] || fail "Provide --user-bin with SUDO_USER when running as root."
        manager_user="$SUDO_USER"
    else
        manager_user="${USER:-}"
    fi
fi

manager_bin_dir="$(destination "$user_bin_path")"
manager_path="$manager_bin_dir/nukleus-manager"
manager_uid=""
manager_gid=""
if [[ -z "$install_root" ]]; then
    [[ -n "$manager_user" ]] || fail "Could not resolve the manager installation user."
    manager_uid="$(id -u "$manager_user")"
    manager_gid="$(id -g "$manager_user")"
fi

program_dir="$(destination "/usr/local/libexec/$SERVICE_NAME")"
config_dir="$(destination "/etc/$SERVICE_NAME")"
unit_dir="$(destination "/usr/lib/systemd/system")"
state_dir="$(destination "/var/lib/$SERVICE_NAME")"
token_path="$(destination "/etc/$SERVICE_NAME/token")"
config_path="$(destination "/etc/$SERVICE_NAME/$SERVICE_NAME.conf")"
unit_path="$(destination "/usr/lib/systemd/system/$SERVICE_NAME.service")"
legacy_models_path="$(destination "/var/lib/$SERVICE_NAME/data/kira-canvas/models")"
service_models_path="$(destination "/var/lib/$SERVICE_NAME/data/nukleus/models")"
service_models_parent="$(dirname "$service_models_path")"

# Deployments made before the rename used the kira-service unit, account and
# directories. They are moved, never merged: the account is renamed in place
# so file ownership and supplementary groups survive.
readonly LEGACY_NAME="kira-service"
legacy_config_dir="$(destination "/etc/$LEGACY_NAME")"
legacy_state_dir="$(destination "/var/lib/$LEGACY_NAME")"
legacy_program_dir="$(destination "/usr/local/libexec/$LEGACY_NAME")"
legacy_unit_path="$(destination "/usr/lib/systemd/system/$LEGACY_NAME.service")"
migrated_legacy=false
renamed_user=false
renamed_group=false
existing_config_source="$config_path"
existing_token_source="$token_path"
dry_run_config=""

# Storage groups are derived from the storage trees on a live installation. The
# installer test sets NUKLEUS_INSTALLER_TEST_STORAGE_GROUPS=1 with --root to
# derive them from the staged trees; account changes are then only printed.
derive_storage_groups=false
staged_storage_groups=false
if [[ -z "$install_root" ]]; then
    derive_storage_groups=true
elif [[ "${NUKLEUS_INSTALLER_TEST_STORAGE_GROUPS:-}" == 1 ]]; then
    derive_storage_groups=true
    staged_storage_groups=true
fi

# The storage paths this run will use, resolved before the legacy migration or
# any other change to the host, with the precedence applied after reading
# nukleus.conf below: the command line, then the configured paths.
if "$derive_storage_groups"; then
    checked_models_dir="$models_dir"
    checked_config_dir="$storage_config_dir"
    if [[ -z "$checked_models_dir" ]]; then
        if [[ -e "$legacy_config_dir" ]]; then
            checked_source="$legacy_config_dir/$LEGACY_NAME.conf"
            checked_prefix="KIRA_SERVICE"
        else
            checked_source="$config_path"
            checked_prefix="NUKLEUS"
        fi
        if [[ -f "$checked_source" ]]; then
            checked_models_dir="$(config_value "${checked_prefix}_MODELS_DIR" "$checked_source")"
            checked_config_dir="$(config_value "${checked_prefix}_CONFIG_DIR" "$checked_source")"
        fi
    fi
    checked_storage=()
    [[ -z "$checked_models_dir" ]] || checked_storage+=("$(destination "$checked_models_dir")")
    [[ -z "$checked_models_dir" || -z "$checked_config_dir" ]] \
        || checked_storage+=("$(destination "$checked_config_dir/catalog")")
    check_storage_groups "${checked_storage[@]}"
fi

if [[ -e "$legacy_config_dir" || -e "$legacy_state_dir" ]]; then
    [[ ! -e "$config_dir" && ! -e "$state_dir" ]] \
        || fail "Both $LEGACY_NAME and $SERVICE_NAME deployments exist; move them manually before installing Nukleus."
    migrated_legacy=true
    if [[ -z "$install_root" ]]; then
        if [[ -f "$legacy_unit_path" ]]; then
            run systemctl disable --now "$LEGACY_NAME.service"
        fi
        if id -u "$LEGACY_NAME" >/dev/null 2>&1 && ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
            run usermod --login "$SERVICE_USER" --home "/var/lib/$SERVICE_NAME" "$LEGACY_NAME"
            renamed_user=true
        fi
        if getent group "$LEGACY_NAME" >/dev/null && ! getent group "$SERVICE_GROUP" >/dev/null; then
            run groupmod --new-name "$SERVICE_GROUP" "$LEGACY_NAME"
            renamed_group=true
        fi
    fi
    if [[ -e "$legacy_config_dir" ]]; then
        run mv "$legacy_config_dir" "$config_dir"
        legacy_config_path="$config_dir/$LEGACY_NAME.conf"
        # A dry run moves nothing, so it reads the legacy files where they are.
        "$dry_run" && legacy_config_path="$legacy_config_dir/$LEGACY_NAME.conf"
        if [[ -f "$legacy_config_path" ]]; then
            if "$dry_run"; then
                printf '+ rewrite %q as %q\n' "$legacy_config_path" "$config_path"
                dry_run_config="$(mktemp)"
                sed -e 's/^KIRA_SERVICE_/NUKLEUS_/' \
                    -e "s|^NUKLEUS_TOKEN_FILE=/etc/$LEGACY_NAME/token\$|NUKLEUS_TOKEN_FILE=/etc/$SERVICE_NAME/token|" \
                    "$legacy_config_path" > "$dry_run_config"
                existing_config_source="$dry_run_config"
                existing_token_source="$legacy_config_dir/token"
            else
                (umask 0077; sed -e 's/^KIRA_SERVICE_/NUKLEUS_/' \
                    -e "s|^NUKLEUS_TOKEN_FILE=/etc/$LEGACY_NAME/token\$|NUKLEUS_TOKEN_FILE=/etc/$SERVICE_NAME/token|" \
                    "$legacy_config_path" > "$config_path")
                chmod 0600 "$config_path"
                rm -f "$legacy_config_path"
            fi
        fi
    fi
    if [[ -e "$legacy_state_dir" ]]; then
        [[ ! -e "$legacy_state_dir/data/$LEGACY_NAME" || ! -e "$legacy_state_dir/data/$SERVICE_NAME" ]] \
            || fail "Both data/$LEGACY_NAME and data/$SERVICE_NAME exist in $legacy_state_dir; move them manually before installing Nukleus."
        had_legacy_data=false
        [[ ! -e "$legacy_state_dir/data/$LEGACY_NAME" ]] || had_legacy_data=true
        run mv "$legacy_state_dir" "$state_dir"
        if "$had_legacy_data"; then
            run mv "$state_dir/data/$LEGACY_NAME" "$service_models_parent"
        fi
    fi
    if [[ -f "$legacy_unit_path" ]]; then run rm -f "$legacy_unit_path"; fi
    if [[ -d "$legacy_program_dir" ]]; then run rm -rf "$legacy_program_dir"; fi
fi

existing_listen=""
existing_port=""
existing_token_file=""
existing_models_dir=""
existing_storage_config_dir=""
has_valid_config=false
if [[ -f "$existing_config_source" ]]; then
    existing_listen="$(config_value NUKLEUS_LISTEN "$existing_config_source")"
    existing_port="$(config_value NUKLEUS_PORT "$existing_config_source")"
    existing_token_file="$(config_value NUKLEUS_TOKEN_FILE "$existing_config_source")"
    existing_models_dir="$(config_value NUKLEUS_MODELS_DIR "$existing_config_source")"
    existing_storage_config_dir="$(config_value NUKLEUS_CONFIG_DIR "$existing_config_source")"
    if valid_address "$existing_listen" && valid_port "$existing_port" \
        && [[ "$existing_token_file" == "/etc/$SERVICE_NAME/token" ]]; then
        if is_loopback_address "$existing_listen" || { [[ -f "$existing_token_source" ]] && token_has_content "$(<"$existing_token_source")"; }; then
            has_valid_config=true
        fi
    fi
fi

updating=false
write_configuration=true
port="$DEFAULT_PORT"
if "$has_valid_config"; then
    updating=true
    write_configuration=false
    listen_address="$existing_listen"
    port="$existing_port"
else
    if [[ -z "$listen_address" && -n "$existing_listen" ]] && valid_address "$existing_listen"; then
        listen_address="$existing_listen"
    fi
    if [[ -n "$existing_port" ]] && valid_port "$existing_port"; then
        port="$existing_port"
    fi
    if [[ -z "$token" && -f "$existing_token_source" ]]; then
        token="$(<"$existing_token_source")"
    fi
    if [[ -z "$listen_address" ]]; then
        choose_listen_address
    fi
    valid_address "$listen_address" || fail "The listen address contains unsupported characters."
    ! is_any_address "$listen_address" || fail "The listen address must identify one interface address, not all interfaces."
    if ! is_loopback_address "$listen_address" && ! token_has_content "$token"; then
        if [[ -t 0 ]]; then
            read -r -s -p "Shared LAN passphrase: " token
            printf '\n'
        else
            fail "Provide --token for a LAN binding when input is not interactive."
        fi
    fi
    [[ "$token" != *$'\n'* && "$token" != *$'\r'* ]] || fail "The passphrase must be one line."
    if ! is_loopback_address "$listen_address" && ! token_has_content "$token"; then
        fail "A non-empty shared LAN passphrase is required."
    fi
fi

# Updates keep the configured storage unless new directories are given.
if [[ -z "$models_dir" && -n "$existing_models_dir" ]]; then
    valid_storage_path "$existing_models_dir" && valid_storage_path "$existing_storage_config_dir" \
        || fail "The configured storage directories in $config_path are invalid."
    models_dir="$existing_models_dir"
    storage_config_dir="$existing_storage_config_dir"
fi
storage_changed=false
if [[ "$models_dir" != "$existing_models_dir" || "$storage_config_dir" != "$existing_storage_config_dir" ]]; then
    storage_changed=true
fi

if [[ -z "$install_root" ]]; then
    # A renamed legacy account already exists, although a dry run did not rename it.
    if ! "$renamed_group" && ! getent group "$SERVICE_GROUP" >/dev/null; then
        run groupadd --system "$SERVICE_GROUP"
    fi
    if ! "$renamed_user" && ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
        run useradd --system --gid "$SERVICE_GROUP" --home-dir "/var/lib/$SERVICE_NAME" \
            --create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    fi
    for gpu_group in render video; do
        if getent group "$gpu_group" >/dev/null; then
            run usermod --append --groups "$gpu_group" "$SERVICE_USER"
        fi
    done
fi

if [[ -e "$legacy_models_path" ]]; then
    [[ ! -e "$service_models_path" ]] || fail "Both legacy and service model directories exist; move them manually before updating Nukleus."
    run install -d -m 0750 "$(dirname "$service_models_path")"
    if [[ -z "$install_root" && "$updating" == true ]]; then
        run systemctl stop "$SERVICE_NAME.service"
    fi
    run mv "$legacy_models_path" "$service_models_path"
fi

run install -d -m 0755 "$program_dir" "$config_dir" "$unit_dir"
run install -d -m 0750 "$state_dir"
run install -m 0755 "$binary_path" "$program_dir/nukleusd"
if [[ -z "$install_root" ]]; then
    run install -d -o "$manager_uid" -g "$manager_gid" -m 0755 "$manager_bin_dir"
    run install -o "$manager_uid" -g "$manager_gid" -m 0755 "$manager_binary_path" "$manager_path"
else
    run install -d -m 0755 "$manager_bin_dir"
    run install -m 0755 "$manager_binary_path" "$manager_path"
fi

# The application-menu entry, icons and optional desktop shortcut belong to
# the manager user. Directories are created as that user so that no missing
# parent under the home directory becomes root-owned.
as_manager() {
    if [[ -z "$install_root" ]]; then run runuser -u "$manager_user" -- "$@"; else run "$@"; fi
}
install_user_file() {
    if [[ -z "$install_root" ]]; then
        run install -o "$manager_uid" -g "$manager_gid" -m "$1" "$2" "$3"
    else
        run install -m "$1" "$2" "$3"
    fi
}
user_data_path="$(dirname -- "$user_bin_path")/share"
applications_dir="$(destination "$user_data_path/applications")"
icon_root="$(destination "$user_data_path/icons/hicolor")"
if [[ -z "$desktop_dir" && -z "$install_root" ]] && "$desktop_shortcut"; then
    desktop_dir="$(runuser -u "$manager_user" -- xdg-user-dir DESKTOP 2>/dev/null || true)"
    [[ -n "$desktop_dir" && "$desktop_dir" != "$(getent passwd "$manager_user" | cut -d: -f6)" ]] || desktop_dir=""
fi
desktop_entry="$(mktemp)"
trap 'rm -f "$desktop_entry" "$dry_run_config"' EXIT
# An absolute icon path avoids depending on icon-theme caches and inheritance.
sed -e "s|@NUKLEUS_MANAGER@|$user_bin_path/nukleus-manager|g" \
    -e "s|@NUKLEUS_MANAGER_ICON@|$user_data_path/icons/hicolor/256x256/apps/nukleus-manager.png|g" \
    "$project_root/packaging/desktop/nukleus-manager.desktop.in" > "$desktop_entry"
as_manager install -d -m 0755 "$applications_dir"
install_user_file 0644 "$desktop_entry" "$applications_dir/nukleus-manager.desktop"
for size in 16 32 48 64 128 256 512; do
    as_manager install -d -m 0755 "$icon_root/${size}x${size}/apps"
    install_user_file 0644 "$project_root/assets/icons/nukleus-manager-$size.png" \
        "$icon_root/${size}x${size}/apps/nukleus-manager.png"
done
desktop_shortcut_path=""
if "$desktop_shortcut" && [[ -n "$desktop_dir" && -d "$(destination "$desktop_dir")" ]]; then
    # Desktop shells only launch shortcuts the user trusts through the executable bit.
    desktop_shortcut_path="$(destination "$desktop_dir")/nukleus-manager.desktop"
    install_user_file 0755 "$desktop_entry" "$desktop_shortcut_path"
fi
# Remove the manager files installed under the former Kira Service name.
for legacy_user_file in "$manager_bin_dir/kira-service-manager" \
    "$applications_dir/kira-service-manager.desktop" "$icon_root"/*/apps/kira-service-manager.png; do
    if [[ -f "$legacy_user_file" ]]; then run rm -f "$legacy_user_file"; fi
done
legacy_desktop_dir="$desktop_dir"
if [[ -z "$legacy_desktop_dir" && -z "$install_root" ]]; then
    legacy_desktop_dir="$(runuser -u "$manager_user" -- xdg-user-dir DESKTOP 2>/dev/null || true)"
fi
legacy_shortcut="$(destination "$legacy_desktop_dir")/kira-service-manager.desktop"
if [[ -n "$legacy_desktop_dir" && -f "$legacy_shortcut" ]] \
    && grep -qx 'StartupWMClass=kira-service-manager' "$legacy_shortcut"; then
    run rm -f "$legacy_shortcut"
fi
if [[ -z "$install_root" ]]; then
    command -v update-desktop-database >/dev/null 2>&1 && as_manager update-desktop-database "$applications_dir" || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && as_manager gtk-update-icon-cache -q -t "$icon_root" || true
fi

if [[ -z "$install_root" ]]; then
    run chown "$SERVICE_USER:$SERVICE_GROUP" "$state_dir"
    if [[ -d "$service_models_parent" ]]; then
        # Legacy model trees may have been created by a root-run installer.
        # The daemon must own the complete service model subtree so it can
        # create, update, and remove models after an upgrade.
        run chown -R "$SERVICE_USER:$SERVICE_GROUP" "$service_models_parent"
    fi
fi

# Shared storage keeps its group, gains setgid group write and is never
# chowned recursively; private storage belongs to the service account.
storage_groups=()
prepare_storage() {
    local group
    if ! "$derive_storage_groups"; then
        run install -d -m 0750 "${@/#/$install_root}"
        return
    fi
    group="$(storage_group_for "$(destination "$1")")"
    if [[ "$group" == "$SERVICE_GROUP" ]]; then
        if "$staged_storage_groups"; then
            run install -d -m 0750 "${@/#/$install_root}"
        else
            run install -d -o "$SERVICE_USER" -g "$SERVICE_GROUP" -m 0750 "$@"
        fi
    else
        run install -d -g "$group" -m 2775 "${@/#/$install_root}"
        [[ " ${storage_groups[*]} " == *" $group "* ]] || storage_groups+=("$group")
    fi
}
if [[ -n "$models_dir" ]]; then
    prepare_storage "$models_dir" "$models_dir"/{generation,upscale,llm,companion}
    prepare_storage "$storage_config_dir/catalog"
    for storage_group in "${storage_groups[@]}"; do
        if "$staged_storage_groups"; then
            print_action usermod --append --groups "$storage_group" "$SERVICE_USER"
        else
            run usermod --append --groups "$storage_group" "$SERVICE_USER"
        fi
    done
fi

if "$write_configuration" || "$storage_changed"; then
    if "$dry_run"; then
        printf '+ write %q\n' "$config_path"
    else
        umask 0077
        {
            printf 'NUKLEUS_LISTEN=%s\nNUKLEUS_PORT=%s\nNUKLEUS_TOKEN_FILE=/etc/%s/token\n' \
                "$listen_address" "$port" "$SERVICE_NAME"
            if [[ -n "$models_dir" ]]; then
                printf 'NUKLEUS_MODELS_DIR=%s\nNUKLEUS_CONFIG_DIR=%s\n' "$models_dir" "$storage_config_dir"
            fi
        } > "$config_path"
        chmod 0600 "$config_path"
    fi
    [[ -n "$install_root" ]] || run chown root:root "$config_path"
fi
if "$write_configuration"; then
    if "$dry_run"; then
        printf '+ write %q\n' "$token_path"
    else
        umask 0077
        printf '%s\n' "$token" > "$token_path"
        chmod 0640 "$token_path"
    fi
    [[ -n "$install_root" ]] || run chown "root:$SERVICE_GROUP" "$token_path"
fi

gpu_groups=""
if [[ -z "$install_root" ]]; then
    for gpu_group in render video; do
        if getent group "$gpu_group" >/dev/null; then
            gpu_groups+=" $gpu_group"
        fi
    done
fi
for storage_group in "${storage_groups[@]}"; do
    gpu_groups+=" $storage_group"
done
gpu_line=""
if [[ -n "$gpu_groups" ]]; then
    gpu_line="SupplementaryGroups=${gpu_groups# }"
fi
storage_mounts=""
storage_args=""
storage_paths=""
if [[ -n "$models_dir" ]]; then
    storage_mounts="RequiresMountsFor=$models_dir $storage_config_dir"
    storage_args=' --models-dir ${NUKLEUS_MODELS_DIR} --config-dir ${NUKLEUS_CONFIG_DIR}'
    storage_paths=" $models_dir $storage_config_dir/catalog"
fi
if "$dry_run"; then
    printf '+ render %q\n' "$unit_path"
else
    sed -e "s|@NUKLEUS_GPU_GROUPS@|$gpu_line|" \
        -e "s|@NUKLEUS_STORAGE_MOUNTS@|$storage_mounts|" \
        -e "s|@NUKLEUS_STORAGE_ARGS@|$storage_args|" \
        -e "s|@NUKLEUS_STORAGE_PATHS@|$storage_paths|" \
        "$project_root/packaging/systemd/$SERVICE_NAME.service.in" > "$unit_path"
    chmod 0644 "$unit_path"
fi

if [[ -z "$install_root" ]] && command -v systemd-analyze >/dev/null 2>&1; then
    run systemd-analyze verify "$unit_path"
fi

if [[ -z "$install_root" && "$enable_service" == true ]]; then
    run systemctl daemon-reload
    if "$updating" && ! "$migrated_legacy"; then
        run systemctl restart "$SERVICE_NAME.service"
    else
        run systemctl enable --now "$SERVICE_NAME.service"
    fi
fi

subnet=""
if command -v ip >/dev/null 2>&1; then
    subnet="$(ip -o -4 addr show 2>/dev/null | awk -v address="$listen_address" '$4 ~ ("^" address "/") { print $4; exit }' || true)"
fi
action="Installed"
if "$updating"; then action="Updated"; fi
printf '\n%s Nukleus.\n' "$action"
printf 'Manage it with: sudo systemctl status|restart|stop|enable --now %s\n' "$SERVICE_NAME"
printf 'Logs: journalctl -u %s -f\n' "$SERVICE_NAME"
if is_loopback_address "$listen_address"; then
    printf 'The service listens only on this computer at 127.0.0.1:%s.\n' "$port"
else
    printf 'The service listens on 127.0.0.1:%s and %s:%s.\n' "$port" "$listen_address" "$port"
    printf 'Firewall rules were not changed. Allow TCP port %s only from your LAN' "$port"
    if [[ -n "$subnet" ]]; then
        printf ' (%s)' "$subnet"
    fi
    printf '.\n'
fi
printf 'Model manager installed at %s. Start the GUI with: %s --gui\n' "$manager_path" "$manager_path"
printf 'Application-menu entry: %s\n' "$applications_dir/nukleus-manager.desktop"
if [[ -n "$desktop_shortcut_path" ]]; then
    printf 'Desktop shortcut: %s\n' "$desktop_shortcut_path"
fi
