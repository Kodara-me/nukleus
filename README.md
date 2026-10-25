# Nukleus

Nukleus is the standalone backend for Kira applications. It owns installed models and their recommended options, single HTTPS model installations, the `sd-cli` image queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

## Install

On Linux, one command builds the latest release, runs its tests and installs it
as a systemd service with the manager in `~/.local/bin`:

```sh
curl -fsSL https://github.com/jstmichel/nukleus/releases/latest/download/get-nukleus.sh | sh
```

It needs curl, git, CMake 3.24+, a C++20 compiler, Qt 6.5+ (Network,
WebSockets, Widgets, SVG, LinguistTools), LibArchive and sudo; Ardoise is cloned by CMake
when it is not installed. The installer asks where the service listens.
Installer options follow `--`, for example a loopback-only service with a
desktop shortcut:

```sh
curl -fsSL https://github.com/jstmichel/nukleus/releases/latest/download/get-nukleus.sh | sh -s -- -- --listen 127.0.0.1 --desktop-shortcut
```

`--version v0.1.0` installs a given release and `--dry-run` stops before
installing. The archive is checked against the release's `SHA256SUMS` and
nothing is installed when it does not match or a test fails. Image work also
needs the runtime tools listed under
[Runtime tools on Arch Linux](#runtime-tools-on-arch-linux).

### Runtime tools on Arch Linux

The daemon runs `sd-cli` for image jobs and llama.cpp's `llama-server` or
`llama-completion` for text jobs. On Arch Linux and derivatives such as
CachyOS, build both with the Vulkan backend from the AUR:

```sh
paru -S llama.cpp-vulkan-git stable-diffusion.cpp-vulkan-git
```

`ffmpeg` is optional: it makes the ping-pong loops and the MP4 output of
`image.animate` (`sudo pacman -S ffmpeg`). The service uses the `ffmpeg` on its PATH, or the one
`NUKLEUS_FFMPEG` names in `/etc/nukleus/nukleus.conf`.

Both packages build from the upstream sources and install into `/usr/bin`,
which is on the systemd unit's `PATH`. stable-diffusion.cpp links its own ggml,
so the packages do not conflict. Restart the service after installing or
updating them, then check that the journal no longer reports a missing
`sd-cli`:

```sh
sudo systemctl restart nukleus
journalctl -u nukleus -n 20
```

The unit sets `ProtectHome=true`, so the service cannot run executables from a
home directory: install manually built tools outside `/home` (for example
`/usr/local/bin`) or point `NUKLEUS_LLAMA_SERVER`/`NUKLEUS_LLAMA_COMPLETION` at such a
path. Manual Vulkan builds of llama.cpp also need the `spirv-headers` package.

## Build

Building requires CMake 3.24 or newer. The manager requires
[Ardoise](https://github.com/jstmichel/ardoise) 0.1, Qt SVG, and LinguistTools
in addition to Qt Network/WebSockets/Widgets. CMake uses an installed Ardoise
package when found and otherwise clones tag `v0.1.0`. Install Ardoise with one
command:

```sh
curl -fsSL https://github.com/jstmichel/ardoise/releases/latest/download/get-ardoise.sh | sh
```

To build against a local Ardoise checkout, pass
`-DFETCHCONTENT_SOURCE_DIR_ARDOISE=/path/to/ardoise`. The daemon and installable
SDK do not link the UI framework.


```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

For a standard CMake prefix installation of the executables:

```sh
cmake --install build --prefix "$HOME/.local"
```

This installs the daemon under `libexec/nukleus/` and the manager under
`bin/`. Use `scripts/install-nukleus.sh` for the complete Linux systemd
installation, including configuration, service-account setup, model storage,
and the user-local manager installation.

Sources are organized under `src/daemon/`, `src/manager/`, and
`src/sdk/`. The executable targets remain declared by
`src/CMakeLists.txt`, so built binaries continue to appear under `build/src/`.

The reusable image client is exported as `Nukleus::SDK`; the namespaced
wire implementation is `Nukleus::Protocol`. Install the static SDK and
headers with `cmake --install build --prefix "$HOME/.local"`, then consume it
with `find_package(NukleusSdk CONFIG REQUIRED)`. The SDK supports image
generation, fixed 2× upscaling, model discovery, progress, and binary results.

## Documentation

- [Protocol v3](docs/protocol-v3.md): the WebSocket protocol reference.
- [Manager](docs/manager.md): the administrator GUI and the CLI.
- [Models](docs/models.md): categories, recommended options, image, animation and companion models, installation sources, storage layout.
- [Jobs](docs/jobs.md): image generation and editing, upscaling, prompt enhancement, text chat, image animation.
- [Presets and routing](docs/presets.md): generation presets, editing qualities and `model-routing.json`.
- [Identity portrait packages](docs/identity-packages.md): the SDK `.kiraid` helper.
- [Deployment](docs/deployment.md): the systemd installer, split storage, remote updates and migrations.
- [CONTRIBUTING.md](CONTRIBUTING.md): building, testing and contributing.

## Upgrading from Kira Service

Nukleus was previously named Kira Service. The installer migrates such a
deployment in place (service, account and group, configuration and data), the
daemon and the manager migrate their own data and settings, and clients built
against the SDK switch to the `NukleusSdk` package names. The protocol and port
`9384` are unchanged. See
[Upgrading from Kira Service](docs/deployment.md#upgrading-from-kira-service)
for the details.

## License

Nukleus is released under the [MIT License](LICENSE).
