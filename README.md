# Nukleus

Nukleus is a Linux service that keeps your local AI models and runs image, video and text jobs on them, one at a time in a single FIFO queue, for any number of client applications over the versioned WebSocket protocol v3. Clients pick an installed model by its opaque ID and submit jobs; they never touch the service's files. [Kaimera](https://github.com/Kodara-me/kaimera) and Kira Canvas are example clients.

## Features

- Image generation, reference-image editing and fixed 2× upscaling, run with `sd-cli`.
- Image animation: a start image (and optionally an end image) becomes a short video, with seamless ping-pong loops and MP4 output.
- Text chat and prompt enhancement with llama.cpp models.
- A model catalogue with recommended options, companion files (VAE, text encoders, vision projectors, LoRAs) and installation from HTTPS URLs or local files.
- Presets and editing qualities shared by every client.
- Progress reporting and binary results over the WebSocket, with the same queue for every client.
- A token-protected LAN listener, or loopback only.

## Architecture

- **`nukleusd`** is the daemon. It owns the models, the queue and the WebSocket API on port `9384`.
- **`nukleus-web`** is a small web service, a client of `nukleusd` like any other, for the local network. It listens on port `9385` and serves a responsive page that shows whether Nukleus is available and, when it is not, why. See [Web service](#web-service).
- **`nukleus-manager`** is the administrator tool, a GUI and a CLI, that installs and configures models and presets.
- **`NukleusSdk`** is the static C++/Qt client library (`Nukleus::SDK`) that speaks the protocol for you. Clients in other languages use [the protocol](docs/protocol-v3.md) directly.

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

### Web service

`nukleus-web` serves a plain HTML page and a small API to any browser on the local
network, with no sign-in: it answers only loopback, private and link-local source
addresses, and only requests whose `Host` names this machine. It connects to
`nukleusd` on port `9384` and reconnects on its own. It is configured by
environment variables:

| Variable | Meaning |
| --- | --- |
| `NUKLEUS_WEB_LISTEN` | A LAN IPv4 address to listen on, besides `127.0.0.1`. Unset: loopback only. |
| `NUKLEUS_WEB_BACKEND_HOST` | The host of `nukleusd`. Default `127.0.0.1`. |
| `NUKLEUS_WEB_BACKEND_TOKEN_FILE` | A file holding the LAN token, required when the backend host is not loopback and never used for loopback. |

From a build, with `nukleusd` running on the same machine:

```sh
./build/src/nukleus-web
```

Then open `http://127.0.0.1:9385/`. `GET /api/status` answers the connection state
(`available`, `unavailable` or `connecting`) and, while unavailable, its reason
(`unreachable`, `authentication-refused` or `incompatible-protocol`).

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

## First steps with the manager

The installer starts the service. Check that it runs, then add a first model with
the manager and list it. This example installs an upscaler; replace the URL with
the model file you want (the manager downloads it and one installation runs at a
time):

```sh
systemctl status nukleus
nukleus-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler --display-name "Upscaler"
nukleus-manager list
```

`nukleus-manager --gui` does the same in a window, with the model options. The
manager connects to the local service without a token. See [Manager](docs/manager.md)
for the GUI, every CLI command and the other categories, and [Models](docs/models.md)
for companions and installation sources.

## Writing a client

A client opens a WebSocket to the service (`ws://127.0.0.1:9384` on the same
machine), authenticates, then asks for the installed models:

```json
{"type":"authenticate","version":3,"token":""}
{"type":"models.list"}
```

Loopback clients may send an empty token; other clients send the configured LAN
token. The service answers `authenticated`, `capabilities` and then
`models.list.result`:

```json
{"type":"models.list.result","categories":[{"category":"upscaler","models":[{"id":"upscaler:model.gguf","name":"model.gguf","fileName":"model.gguf","bytes":67108864,"displayName":"Upscaler","companionFiles":{},"modelType":"checkpoint"}]}]}
```

Jobs then name a model `id` from that list. The complete message reference is in
[Protocol v3](docs/protocol-v3.md).

Clients written in C++ with Qt can use the SDK instead. Install it with
`cmake --install build --prefix "$HOME/.local"`, then in `CMakeLists.txt`:

```cmake
find_package(NukleusSdk CONFIG REQUIRED)
target_link_libraries(my-client PRIVATE Nukleus::SDK)
```

A minimal client that connects and lists the models:

```cpp
#include <nukleus/ServiceClient.h>

#include <QCoreApplication>
#include <QDebug>
#include <QUrl>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    Nukleus::ServiceClient client;
    QObject::connect(&client, &Nukleus::ServiceClient::modelsChanged, &app, [&client, &app] {
        for (const Nukleus::ModelInfo &model : client.models())
            qInfo() << model.id << model.displayLabel();
        app.quit();
    });
    client.setConfiguration(QUrl(QStringLiteral("ws://127.0.0.1:9384")), {});
    client.connectToService();
    return app.exec();
}
```

The SDK authenticates and requests the models by itself once connected. It also
submits generation, editing, upscaling, animation, chat and prompt-enhancement
jobs and reports their progress and binary results.

## Build

Building from source needs CMake 3.24+, Ninja, a C++20 compiler, Qt 6.5+ (Network,
WebSockets, Widgets, SVG, LinguistTools), LibArchive and
[Ardoise](https://github.com/jstmichel/ardoise) 0.1, which CMake clones when it is
not installed. To install Ardoise first, with one command:

```sh
curl -fsSL https://github.com/jstmichel/ardoise/releases/latest/download/get-ardoise.sh | sh
```

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

`cmake --install build --prefix "$HOME/.local"` installs the daemon under
`libexec/nukleus/`, the manager under `bin/`, and the SDK with its headers. Use
`scripts/install-nukleus.sh` for the complete systemd installation. Building
against a local Ardoise checkout, the source layout and the tests are described in
[CONTRIBUTING.md](CONTRIBUTING.md).

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
