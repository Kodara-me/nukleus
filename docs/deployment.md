# Deployment

How the Linux systemd service is installed, configured, updated and migrated, and
how the Windows services are installed ([Windows](#windows)).
The one-command install and the runtime tools the daemon needs are in the
[README](../README.md#install). Model storage is described in
[Models](models.md#storage-layout).

## Installing the service

```sh
sudo scripts/install-nukleus.sh --install-backend --binary build/src/nukleusd --manager-binary build/src/nukleus-manager
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, installs the manager into the invoking user's `~/.local/bin` with an application-menu entry (`~/.local/share/applications`, icons under `~/.local/share/icons/hicolor` referenced by absolute path); `--desktop-shortcut` also adds a shortcut to the user's XDG desktop directory, or `--desktop-dir PATH` to another directory, and restarts updated installations. A LAN binding listens on both `127.0.0.1` and the selected IPv4 address. Loopback manager clients authenticate without a token; non-loopback clients require the shared LAN token. Configuration remains under `/etc/nukleus/`; system-service model data resolves to `/var/lib/nukleus/data/nukleus/models/`.

To use the split layout, pass `--models-dir DIR --config-dir DIR` to the installer (or after `--` to the updater). The installer stores both in `nukleus.conf`, keeps them on later updates, creates the category and `catalog/` directories, and restricts the unit's `ReadWritePaths=` to them. When a storage directory belongs to a non-root group (for example a shared `/srv/lab` owned by `users`), the service joins that group instead of taking ownership. When a storage directory belongs to a group id that has no name on the host (for example after a system update renumbered its group), the installer stops before changing anything, names each directory and its group id, and suggests giving the tree a named group (`find TREE -gid ID -exec chgrp -h GROUP {} +`) or creating a group with that id (`groupadd -g ID NAME`). Copy an existing model tree first with:

```bash
scripts/migrate-nukleus-storage.sh --source OLD_MODELS --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus --dry-run
```

Uninstalling, even with `--purge`, never removes the split storage directories.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Choosing the components

The installer handles two components, each chosen by a flag:

| Flag | Installs |
| --- | --- |
| `--install-backend` | `nukleusd` with its `nukleus.service` unit, and the manager. Needs `--binary`, `--manager-binary` and a manager user (`SUDO_USER` or `--user-bin`). |
| `--install-web-service` | `nukleus-web` with its `nukleus-web.service` unit and the unprivileged `nukleus-web` account. Needs `--web-binary` (default `build/src/nukleus-web`) only. |

A component is installed on a host when its unit is present under `/usr/lib/systemd/system`
(`nukleus.service`, or the legacy `kira-service.service`, and `nukleus-web.service`). Every
installed component is updated by every run, with or without a flag, and a flag adds its
component; the installer never adds a component that no flag names, so no update turns on the
web service, which has no sign-in. A host with nothing installed and no flag stops before any
change, naming both flags. `update-nukleus.sh` and `get-nukleus.sh` pass `--web-binary` with
the built `nukleus-web` and accept the flags after `--`.

### Web service on its own host

On a NAS with no GPU, install only the web service and point it at the machine that runs `nukleusd`:

```sh
sudo scripts/install-nukleus.sh --install-web-service --web-binary build/src/nukleus-web \
    --web-listen 192.168.1.20 --web-backend-host 192.168.1.10 --web-backend-token 'LAN TOKEN'
```

- `--web-listen ADDRESS` is the LAN IPv4 address of the interface (`127.0.0.1` for this
  computer only). Interactively the installer offers the same list of addresses as for the
  backend; without a terminal the option is required.
- `--web-backend-host` and `--web-backend-token` are required, or asked (the token with a
  masked prompt), unless the backend is installed on the same host or by the same run, in
  which case the web service connects over loopback and asks for neither. The token is the
  backend's LAN token.
- The configuration is `/etc/nukleus/nukleus-web.conf` (`NUKLEUS_WEB_LISTEN`,
  `NUKLEUS_WEB_BACKEND_HOST`, `NUKLEUS_WEB_BACKEND_TOKEN_FILE`) and the token is stored in
  `/etc/nukleus/web-backend-token`, owned by root, group `nukleus-web`, mode `0640`. Later
  runs keep both. The service listens on port `9385`; firewall rules are not changed.
- `scripts/uninstall-nukleus.sh` removes the web service's program and unit, and with
  `--purge` its configuration and token file. It deletes no account, the `nukleus-web` one
  included.

## Windows

On Windows 11 the daemon and the web service run as Windows services, installed from a build
directory by `scripts\install-nukleus.ps1` and removed by `scripts\uninstall-nukleus.ps1`, both
from an elevated PowerShell.

### Prerequisites

- A build (see [Build on Windows](../README.md#build-on-windows)): the executables are read from
  `<BuildDir>\src` (default `build`), and the DLLs next to them are copied. `windeployqt` from
  `-QtDir` (default: the Qt directory of `<BuildDir>\CMakeCache.txt`) adds the Qt runtime, so the
  installed programs need neither the Qt nor the vcpkg directory on `PATH`.
- The runtime tools, installed machine-wide: `sd-cli`, `llama-server`, `llama-completion` and
  `ffmpeg`. A service sees neither the installing user's `PATH` nor a user's profile, so the
  installer stores their absolute paths, taken from `-SdCli`, `-LlamaServer`, `-LlamaCompletion`
  and `-Ffmpeg` or else found on the installing session's `PATH`. It stops before any change when
  a path is under `C:\Users\...` (for llama.cpp, `winget install ggml.llamacpp --scope machine`),
  and warns, without stopping, about a tool it cannot find.

### Installing and updating

```powershell
scripts\install-nukleus.ps1 -InstallBackend -InstallWebService -BuildDir build-msvc -QtDir C:\Qt\6.8.3\msvc2022_64
```

The component rule is the Linux one: every component already installed is updated, a component
that is not installed is added only by its flag, and a machine with nothing installed needs
`-InstallBackend` (`nukleusd` and the manager), `-InstallWebService` (`nukleus-web`) or both.

| Option | Meaning |
| --- | --- |
| `-BuildDir`, `-QtDir` | Where the executables are, and the Qt installation for `windeployqt`. |
| `-Listen ADDRESS` | LAN IPv4 address of the backend, added to the always-present `127.0.0.1`, on port `9384`. Needs a token. |
| `-Token TOKEN` | LAN token, stored in `config\token`. Without it, the existing token is kept, and an interactive session gets a masked prompt; a non-interactive one stops before any change. |
| `-ModelsDir DIR`, `-ConfigDir DIR` | Another weight directory and another configuration directory (the catalogue is `<ConfigDir>\catalog\`), always together. Created when missing, never emptied, their permissions kept apart from write access for the daemon's account. The token stays in `%ProgramData%\Nukleus\config\`. |
| `-SdCli`, `-LlamaServer`, `-LlamaCompletion`, `-Ffmpeg` | Paths of the runtime tools. |
| `-WebListen ADDRESS` | LAN IPv4 address of the web interface, on port `9385`. |
| `-WebBackendHost HOST`, `-WebBackendToken TOKEN` | A backend on another machine; the token is stored in `web\backend-token`. Without them the web service reaches the backend of this machine over loopback. |
| `-NoStart` | Install without starting the services. |
| `-DryRun` | Print each action and change nothing (no elevation needed). |

With no `-Listen`, a first installation listens on loopback only. Each service is registered
with automatic start, runs as its own virtual account (`NT SERVICE\nukleus`,
`NT SERVICE\nukleus-web`) and restarts 5 seconds after a failure. The backend's command line
(`--listen`, `--port`, `--token-file`, `--models-dir`, `--config-dir`) and the `Environment`
value of each service (tool paths, `NUKLEUS_LOG_FILE`, `TEMP` and `TMP` for the backend,
`NUKLEUS_WEB_*` for the web service) are held by the Service Control Manager, and the installer
is their only writer. A LAN listener gets an inbound Windows Firewall rule on the Private profile,
scoped to the service's executable (`Nukleus`, `Nukleus web interface`); loopback has none, and a
rule left by an earlier run is removed. The backend installation adds an all-users Start menu
shortcut, "Nukleus Manager", that starts the manager with `--gui`.

Running the installer again with a new build stops the services, replaces the executables and
DLLs, keeps the listen addresses, tokens, storage directories and tool paths that no option
changes, and starts the services again. It never modifies models or the catalogue.

### Data, logs and permissions

Everything the services keep is under `%ProgramData%\Nukleus`:

| Directory | Content | Access |
| --- | --- | --- |
| `config\` | The token, and by default the catalogue in `config\catalog\` | Administrators, SYSTEM, the daemon's account |
| `models\` | The default weight directory | The daemon's account writes |
| `tmp\` | The backend's `TEMP` and `TMP` (job files, the llama-server key file) | Administrators, SYSTEM, the daemon's account |
| `web\` | The web service's backend token | Administrators, SYSTEM, the web service's account |
| `logs\` | `nukleusd.log` and `nukleus-web.log`, renamed with a `.1` suffix at 10 MiB | Administrators, SYSTEM, both accounts |

### Checking the services

```powershell
Get-Service nukleus, nukleus-web
Get-Content "$env:ProgramData\Nukleus\logs\nukleusd.log" -Tail 20
```

Starts, stops and crashes are also in the System event log. A start failure is logged in the
service's log file and reported to the Service Control Manager as a nonzero service-specific
exit code.

### Removing

```powershell
scripts\uninstall-nukleus.ps1
```

stops and deletes both services and removes their firewall rules, the Start menu shortcut and
`%ProgramFiles%\Nukleus`. It keeps `%ProgramData%\Nukleus` and any directory given by
`-ModelsDir` or `-ConfigDir`, and prints their paths. `-Purge` also removes
`%ProgramData%\Nukleus`, after a prompt or with `-Yes`, and never a directory outside it.
`-DryRun` prints the actions only.

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
