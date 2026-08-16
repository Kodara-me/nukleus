# Kira Service

Kira Service is the standalone backend for Kira applications. It owns installed models, Hugging Face downloads, the `sd-cli` execution queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

`kira-service` is the daemon. `kira-service-manager` is a native Qt administrator app intended to run on the service host. It connects to localhost, or an IPv4 address assigned to that host, at port `9384` and uses the configured shared token.

## Models

Service models live in `$XDG_DATA_HOME/kira-service/models/`: image-generation files in `models/`, upscalers in `models/upscale/`, and LLM chat files in `models/llm/`. The manager installs direct HTTPS Hugging Face `resolve` URLs, optionally verifies SHA-256, and never overwrites an existing filename.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Linux system service

```sh
sudo scripts/install-kira-service.sh --binary build/src/kira-service
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, and restarts updated installations. LAN bindings require a shared token. Configuration remains under `/etc/kira-service/`; system-service model data resolves to `/var/lib/kira-service/data/kira-service/models/`.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/protocol-v3.md](docs/protocol-v3.md).
