# Deployment

How the Linux systemd service is installed, configured, updated and migrated.
The one-command install and the runtime tools the daemon needs are in the
[README](../README.md#install). Model storage is described in
[Models](models.md#storage-layout).

## Installing the service

```sh
sudo scripts/install-nukleus.sh --binary build/src/nukleusd --manager-binary build/src/nukleus-manager
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, installs the manager into the invoking user's `~/.local/bin` with an application-menu entry (`~/.local/share/applications`, icons under `~/.local/share/icons/hicolor` referenced by absolute path); `--desktop-shortcut` also adds a shortcut to the user's XDG desktop directory, or `--desktop-dir PATH` to another directory, and restarts updated installations. A LAN binding listens on both `127.0.0.1` and the selected IPv4 address. Loopback manager clients authenticate without a token; non-loopback clients require the shared LAN token. Configuration remains under `/etc/nukleus/`; system-service model data resolves to `/var/lib/nukleus/data/nukleus/models/`.

To use the split layout, pass `--models-dir DIR --config-dir DIR` to the installer (or after `--` to the updater). The installer stores both in `nukleus.conf`, keeps them on later updates, creates the category and `catalog/` directories, and restricts the unit's `ReadWritePaths=` to them. When a storage directory belongs to a non-root group (for example a shared `/srv/lab` owned by `users`), the service joins that group instead of taking ownership. When a storage directory belongs to a group id that has no name on the host (for example after a system update renumbered its group), the installer stops before changing anything, names each directory and its group id, and suggests giving the tree a named group (`find TREE -gid ID -exec chgrp -h GROUP {} +`) or creating a group with that id (`groupadd -g ID NAME`). Copy an existing model tree first with:

```bash
scripts/migrate-nukleus-storage.sh --source OLD_MODELS --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus --dry-run
```

Uninstalling, even with `--purge`, never removes the split storage directories.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Upgrading from Kira Service

Nukleus was previously named Kira Service. The installer migrates such a
deployment in place: it stops and disables `kira-service.service`, renames the
`kira-service` account and group to `nukleus` (keeping their IDs, so file
ownership and supplementary groups are unchanged), moves `/etc/kira-service` to
`/etc/nukleus` with `KIRA_SERVICE_*` settings rewritten as `NUKLEUS_*`, moves
`/var/lib/kira-service` (including `data/kira-service/`) to
`/var/lib/nukleus` (`data/nukleus/`), removes the old unit and program, and
enables `nukleus.service`. Split storage directories keep their paths. If both
deployments exist, the installer stops without changing either.

Outside systemd, the daemon renames `$XDG_DATA_HOME/kira-service/` to
`$XDG_DATA_HOME/nukleus/` at startup and refuses to start if both exist. The
manager copies its saved connection and window settings from the former
`Kira Service Manager` settings when it has none of its own; the old file is
kept. Clients built against the SDK switch from `KiraServiceSdk`,
`KiraService::SDK` and `<kira/service/…>` to `NukleusSdk`, `Nukleus::SDK` and
`<nukleus/…>`. The protocol and port `9384` are unchanged.

See [CONTRIBUTING.md](../CONTRIBUTING.md) and [protocol-v3.md](protocol-v3.md).

## Remote Linux updates

From another machine, run the updater over SSH on the service host:

```sh
ssh user@service-host '/path/to/nukleus/scripts/update-nukleus.sh'
```

Run as the checkout owner with cached or passwordless sudo credentials. The script
fetches and fast-forwards the current branch's upstream, configures and builds,
runs CTest, then uses the existing systemd installer to preserve configuration and
restart the service. Tracked changes, diverged branches, and build/test failures
stop the update before installation. The restart interrupts active jobs.
Build dependencies must already be available (Ardoise is used from its installed
package or cloned by CMake at the pinned tag); sibling checkouts and `sd-cli` are not updated. See `--help` for build and installer options.
