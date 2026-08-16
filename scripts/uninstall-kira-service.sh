#!/usr/bin/env bash

set -euo pipefail

readonly SERVICE_NAME="kira-service"
install_root=""
dry_run=false
purge=false
yes=false

usage() {
    cat <<'EOF'
Usage: sudo scripts/uninstall-kira-service.sh [options]

Remove the installed Kira Service program and systemd unit. Configuration and
downloaded model data are retained unless --purge is confirmed.

Options:
  --root PATH  Remove files staged under PATH instead of the live filesystem
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
    --purge) purge=true; shift ;;
    --yes) yes=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "Unknown option: $1" ;;
    esac
done

[[ -n "$install_root" || $EUID -eq 0 ]] || fail "Run with sudo, or use --root for a staging installation."

if [[ -z "$install_root" ]]; then
    run systemctl disable --now "$SERVICE_NAME.service" || true
fi
run rm -f "$(destination "/usr/lib/systemd/system/$SERVICE_NAME.service")"
run rm -rf "$(destination "/usr/local/libexec/$SERVICE_NAME")"
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
