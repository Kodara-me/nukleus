#!/usr/bin/env bash

set -euo pipefail

readonly SERVICE_NAME="kira-service"
readonly SERVICE_USER="kira-service"
readonly SERVICE_GROUP="kira-service"
readonly DEFAULT_PORT=9384

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_dir/.." && pwd)"
binary_path="$project_root/build/src/kira-service"
manager_binary_path="$project_root/build/src/kira-service-manager"
listen_address=""
token=""
install_root=""
user_bin_path=""
manager_user=""
dry_run=false
enable_service=true

usage() {
    cat <<'EOF'
Usage: sudo scripts/install-kira-service.sh [options]

Install or update the Kira Service systemd unit and local model manager.
Fresh interactive installs offer loopback-only binding and detected LAN IPv4
addresses. A LAN address also keeps the service available on loopback.

Options:
  --binary PATH       Built kira-service executable (default: build/src/kira-service)
  --manager-binary PATH
                      Built kira-service-manager executable (default: build/src/kira-service-manager)
  --user-bin PATH     User-local bin directory for kira-service-manager
  --listen ADDRESS    LAN bind address, or 127.0.0.1 for local-only service
  --token TOKEN       Shared LAN passphrase; avoid this option in a shared shell history
  --root PATH         Stage files under PATH instead of the live filesystem
  --dry-run           Print actions without modifying the filesystem
  --no-enable         Install without enabling or starting the systemd service
  -h, --help          Show this help
EOF
}

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

run() {
    if "$dry_run"; then
        printf '+ '
        printf '%q ' "$@"
        printf '\n'
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

    printf 'Choose where Kira Service listens:\n'
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
    --root) install_root="${2:-}"; shift 2 ;;
    --dry-run) dry_run=true; shift ;;
    --no-enable) enable_service=false; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "Unknown option: $1" ;;
    esac
done

[[ -n "$install_root" || $EUID -eq 0 ]] || fail "Run with sudo, or use --root for a staging installation."
[[ -x "$binary_path" ]] || fail "Kira Service executable is not executable: $binary_path"
[[ -x "$manager_binary_path" ]] || fail "Kira Service Manager executable is not executable: $manager_binary_path"
[[ -f "$project_root/packaging/systemd/kira-service.service.in" ]] || fail "Systemd unit template is missing."

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
manager_path="$manager_bin_dir/kira-service-manager"
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
service_models_path="$(destination "/var/lib/$SERVICE_NAME/data/kira-service/models")"
service_models_parent="$(dirname "$service_models_path")"

existing_listen=""
existing_port=""
existing_token_file=""
has_valid_config=false
if [[ -f "$config_path" ]]; then
    existing_listen="$(config_value KIRA_SERVICE_LISTEN "$config_path")"
    existing_port="$(config_value KIRA_SERVICE_PORT "$config_path")"
    existing_token_file="$(config_value KIRA_SERVICE_TOKEN_FILE "$config_path")"
    if valid_address "$existing_listen" && valid_port "$existing_port" \
        && [[ "$existing_token_file" == "/etc/$SERVICE_NAME/token" ]]; then
        if is_loopback_address "$existing_listen" || { [[ -f "$token_path" ]] && token_has_content "$(<"$token_path")"; }; then
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
    if [[ -z "$token" && -f "$token_path" ]]; then
        token="$(<"$token_path")"
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

if [[ -z "$install_root" ]]; then
    if ! getent group "$SERVICE_GROUP" >/dev/null; then
        run groupadd --system "$SERVICE_GROUP"
    fi
    if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
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
    [[ ! -e "$service_models_path" ]] || fail "Both legacy and service model directories exist; move them manually before updating Kira Service."
    run install -d -m 0750 "$(dirname "$service_models_path")"
    if [[ -z "$install_root" && "$updating" == true ]]; then
        run systemctl stop "$SERVICE_NAME.service"
    fi
    run mv "$legacy_models_path" "$service_models_path"
fi

run install -d -m 0755 "$program_dir" "$config_dir" "$unit_dir"
run install -d -m 0750 "$state_dir"
run install -m 0755 "$binary_path" "$program_dir/$SERVICE_NAME"
if [[ -z "$install_root" ]]; then
    run install -d -o "$manager_uid" -g "$manager_gid" -m 0755 "$manager_bin_dir"
    run install -o "$manager_uid" -g "$manager_gid" -m 0755 "$manager_binary_path" "$manager_path"
else
    run install -d -m 0755 "$manager_bin_dir"
    run install -m 0755 "$manager_binary_path" "$manager_path"
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

if "$write_configuration"; then
    if "$dry_run"; then
        printf '+ write %q\n' "$config_path"
    else
        umask 0077
        printf 'KIRA_SERVICE_LISTEN=%s\nKIRA_SERVICE_PORT=%s\nKIRA_SERVICE_TOKEN_FILE=/etc/%s/token\n' \
            "$listen_address" "$port" "$SERVICE_NAME" > "$config_path"
        printf '%s\n' "$token" > "$token_path"
        chmod 0600 "$config_path"
        chmod 0640 "$token_path"
    fi
    if [[ -z "$install_root" ]]; then
        run chown root:root "$config_path"
        run chown "root:$SERVICE_GROUP" "$token_path"
    fi
fi

gpu_groups=""
if [[ -z "$install_root" ]]; then
    for gpu_group in render video; do
        if getent group "$gpu_group" >/dev/null; then
            gpu_groups+=" $gpu_group"
        fi
    done
fi
gpu_line=""
if [[ -n "$gpu_groups" ]]; then
    gpu_line="SupplementaryGroups=${gpu_groups# }"
fi
if "$dry_run"; then
    printf '+ render %q\n' "$unit_path"
else
    sed "s|@KIRA_SERVICE_GPU_GROUPS@|$gpu_line|" \
        "$project_root/packaging/systemd/$SERVICE_NAME.service.in" > "$unit_path"
    chmod 0644 "$unit_path"
fi

if [[ -z "$install_root" ]] && command -v systemd-analyze >/dev/null 2>&1; then
    run systemd-analyze verify "$unit_path"
fi

if [[ -z "$install_root" && "$enable_service" == true ]]; then
    run systemctl daemon-reload
    if "$updating"; then
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
printf '\n%s Kira Service.\n' "$action"
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
