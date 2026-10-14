#!/usr/bin/env bash

set -euo pipefail

readonly SERVICE_NAME="kira-service"
install_root=""
user_bin_path=""
desktop_dir_override=""
dry_run=false
purge=false
yes=false

usage() {
    cat <<'EOF'
Usage: sudo scripts/uninstall-kira-service.sh [options]

Remove the installed Kira Service program, model manager, and systemd unit.
Configuration and downloaded model data are retained unless --purge is confirmed.

Options:
  --root PATH  Remove files staged under PATH instead of the live filesystem
  --user-bin PATH
               User-local bin directory containing kira-service-manager
  --desktop-dir PATH
               Directory holding the manager desktop shortcut
               (default: the user's XDG desktop directory)
  --purge      Also remove /etc/kira-service and /var/lib/kira-service
  --yes        Confirm --purge without prompting
  --dry-run    Print actions without modifying the filesystem
  -h, --help   Show this help
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

while (( $# > 0 )); do
    case "$1" in
    --root) install_root="${2:-}"; shift 2 ;;
    --user-bin) user_bin_path="${2:-}"; shift 2 ;;
    --desktop-dir) desktop_dir_override="${2:-}"; shift 2 ;;
    --purge) purge=true; shift ;;
    --yes) yes=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "Unknown option: $1" ;;
    esac
done

[[ -n "$install_root" || $EUID -eq 0 ]] || fail "Run with sudo, or use --root for a staging installation."

manager_user=""
if [[ -z "$user_bin_path" ]]; then
    if [[ "$EUID" -eq 0 ]]; then
        [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]] || fail "Provide --user-bin when running as root without SUDO_USER."
        manager_user="$SUDO_USER"
        user_home="$(getent passwd "$manager_user" | cut -d: -f6)"
        [[ -n "$user_home" ]] || fail "Could not resolve the home directory for $manager_user."
        user_bin_path="$user_home/.local/bin"
    else
        user_bin_path="${HOME:?}/.local/bin"
    fi
else
    [[ "$user_bin_path" == /* ]] || fail "--user-bin must be an absolute path."
    [[ "$EUID" -ne 0 || "${SUDO_USER:-root}" == root ]] || manager_user="$SUDO_USER"
fi

if [[ -z "$install_root" ]]; then
    run systemctl disable --now "$SERVICE_NAME.service" || true
fi
run rm -f "$(destination "/usr/lib/systemd/system/$SERVICE_NAME.service")"
run rm -rf "$(destination "/usr/local/libexec/$SERVICE_NAME")"
run rm -f "$(destination "$user_bin_path/kira-service-manager")"
user_data_path="$(dirname -- "$user_bin_path")/share"
run rm -f "$(destination "$user_data_path/applications/kira-service-manager.desktop")"
for size in 16 32 48 64 128 256 512; do
    run rm -f "$(destination "$user_data_path/icons/hicolor/${size}x${size}/apps/kira-service-manager.png")"
done
desktop_dir="$desktop_dir_override"
if [[ -z "$desktop_dir" && -z "$install_root" && -n "$manager_user" ]]; then
    desktop_dir="$(runuser -u "$manager_user" -- xdg-user-dir DESKTOP 2>/dev/null || true)"
fi
shortcut="$(destination "$desktop_dir")/kira-service-manager.desktop"
# Only remove a shortcut that this installer created.
if [[ -n "$desktop_dir" && -f "$shortcut" ]] && grep -qx 'StartupWMClass=kira-service-manager' "$shortcut"; then
    run rm -f "$shortcut"
fi
if [[ -z "$install_root" ]]; then
    run systemctl daemon-reload
fi

if "$purge" && ! "$yes"; then
    read -r -p "Remove configuration and downloaded models under /etc/$SERVICE_NAME and /var/lib/$SERVICE_NAME? [y/N] " answer
    [[ "$answer" =~ ^[Yy]$ ]] || purge=false
fi
if "$purge"; then
    run rm -rf "$(destination "/etc/$SERVICE_NAME")" "$(destination "/var/lib/$SERVICE_NAME")"
fi

printf 'Kira Service program and systemd unit removed.\n'
if ! "$purge"; then
    printf 'Configuration and model data were retained. Re-run with --purge to remove them.\n'
fi
