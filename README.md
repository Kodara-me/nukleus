# Kira Service

Kira Service is the standalone backend for Kira applications. It owns installed models, Hugging Face downloads, the `sd-cli` execution queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

For a standard CMake prefix installation of the executables:

```sh
cmake --install build --prefix "$HOME/.local"
```

This installs the daemon under `libexec/kira-service/` and the manager under
`bin/`. Use `scripts/install-kira-service.sh` for the complete Linux systemd
installation, including configuration, service-account setup, model storage,
and the user-local manager installation.

Sources are organized under `src/daemon/`, `src/manager/`, and
`src/protocol/`. The executable targets remain declared by
`src/CMakeLists.txt`, so built binaries continue to appear under `build/src/`.

`kira-service` is the daemon. `kira-service-manager` is a native Qt administrator app installed into the invoking user's `~/.local/bin`. Start its GUI with `kira-service-manager --gui`; it automatically connects to the local service at `127.0.0.1:9384` and retries while the service is unavailable.

The manager also provides local CLI operations:

```sh
kira-service-manager list
kira-service-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler
kira-service-manager delete upscaler:model.gguf
```

Use `--help` for all options. CLI and GUI manager connections are local-only and do not require manual host or token configuration.

## Models

Service models live in `$XDG_DATA_HOME/kira-service/models/`: image-generation files in `models/`, upscalers in `models/upscale/`, and LLM chat files in `models/llm/`. The manager installs direct HTTPS Hugging Face `resolve` URLs, optionally verifies SHA-256, and never overwrites an existing filename.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Linux system service

```sh
sudo scripts/install-kira-service.sh --binary build/src/kira-service --manager-binary build/src/kira-service-manager
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, installs the manager into the invoking user's `~/.local/bin`, and restarts updated installations. A LAN binding listens on both `127.0.0.1` and the selected IPv4 address. Loopback manager clients authenticate without a token; non-loopback clients require the shared LAN token. Configuration remains under `/etc/kira-service/`; system-service model data resolves to `/var/lib/kira-service/data/kira-service/models/`.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/protocol-v3.md](docs/protocol-v3.md).
