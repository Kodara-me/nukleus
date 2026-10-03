# Kira Service

Kira Service is the standalone backend for Kira applications. It owns installed models and their recommended options, single HTTPS model installations, the `sd-cli` image queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

## Build

The manager requires Kira UI Framework 0.2, Qt SVG, and LinguistTools in
addition to Qt Network/WebSockets/Widgets. By default CMake uses the sibling
`../kira-ui-framework`; set `KIRA_UI_SOURCE_DIR` to another checkout. If that
path is not a checkout, CMake requires the installed `KiraUi` package. The
daemon and installable SDK do not link the UI framework.


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
`src/sdk/`. The executable targets remain declared by
`src/CMakeLists.txt`, so built binaries continue to appear under `build/src/`.

The reusable image client is exported as `KiraService::SDK`; the namespaced
wire implementation is `KiraService::Protocol`. Install the static SDK and
headers with `cmake --install build --prefix "$HOME/.local"`, then consume it
with `find_package(KiraServiceSdk CONFIG REQUIRED)`. The SDK supports image
generation, fixed 2× upscaling, model discovery, progress, and binary results.

`kira-service` is the daemon. `kira-service-manager --gui` opens the local
administrator UI with the shared dark Kira theme: a category/model tree on the
left and editable recommended options on the right. Its default window is
1300 × 680, its minimum is 1000 × 550, and saved geometry takes precedence.
Select a model or its companion to edit recommendations, then Save to persist
them on the service. Unsaved edits prompt before leaving the model.

Add model opens a modal for HTTPS model/companion URLs and category-specific
recommendations. Progress stays in that modal; success closes it and selects
the installed model. Closing during download cancels the installation and
cleans its temporary files. Only one installation is accepted across clients;
there is no download queue. The image execution queue is unchanged.
The manager connects to `127.0.0.1:9384` and retries while the service is offline.

Recommended options are metadata returned to clients, not enforced execution
settings. Image generation/Edition expose square resolution (256–2048 in steps
of 256), steps, CFG scale, sampler, scheduler, and Clip Skip. LLM exposes
temperature, top-p, context length, and maximum tokens; Upscaler exposes tile
size (execution remains fixed at 2×); IP-Adapter exposes strength. Existing
models without recommendations remain usable; proposed defaults are saved only
on an explicit Save. Canvas does not apply these recommendations yet.

The manager also provides local CLI operations:

```sh
kira-service-manager list
kira-service-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler
kira-service-manager add https://huggingface.co/h94/IP-Adapter-FaceID/resolve/main/ip-adapter-faceid-plusv2_sdxl.bin?utm_source=chatgpt.com --category ip-adapter --companion-lora 'https://huggingface.co/h94/IP-Adapter-FaceID/resolve/main/ip-adapter-faceid-plusv2_sdxl_lora.safetensors?utm_source=chatgpt.com'
kira-service-manager add https://example.com/model.gguf --category llm-chat --mmproj https://example.com/mmproj.gguf
kira-service-manager add https://example.com/edit.safetensors --category edition --vae https://example.com/vae.safetensors
kira-service-manager delete upscaler:model.gguf
```

Use `--help` for all options. CLI and GUI manager connections are local-only and do not require manual host or token configuration.

## Models

The `edition` category supports installing, listing, and deleting image-editing models in the manager GUI and CLI (`add <url> --category edition`). It accepts `.safetensors`, `.ckpt`, and `.gguf` files. Editing jobs are not implemented yet.

Service models live in `$XDG_DATA_HOME/kira-service/models/`: image-generation files in `models/`, edition models for image editing in `models/edition/`, upscalers in `models/upscale/`, LLM chat files in `models/llm/`, and IP-Adapters in `models/ip-adapter/`. Optional companion files are kept under each category's private `companions/<primary filename>/` directory and linked through persistent category-local metadata: mmproj for LLM-chat, VAE for Image-generation and Edition, and LoRA for IP-Adapter. The manager downloads and installs each pair together; failed or canceled downloads leave no usable pair, and deleting the primary also removes its companion. Model listings expose `companionFileName` and `companionRole` (`mmproj`, `vae`, or `lora`) without exposing service paths. The manager accepts HTTPS model-file URLs and never overwrites an installed filename. SHA-256 fields and download verification are no longer supported. Primary IP-Adapter files support `.bin`, `.safetensors`, `.ckpt`, and `.gguf`; mmproj files require `.gguf`, while VAE and LoRA files accept `.safetensors`, `.ckpt`, or `.gguf`. Query-bearing URLs are accepted and Hugging Face `blob` links are normalized to `resolve` URLs. The add-model modal shows the optional companion URL field appropriate to the selected category. These attachments are catalog metadata only; image runtime invocation is unchanged.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Linux system service

```sh
sudo scripts/install-kira-service.sh --binary build/src/kira-service --manager-binary build/src/kira-service-manager
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, installs the manager into the invoking user's `~/.local/bin`, and restarts updated installations. A LAN binding listens on both `127.0.0.1` and the selected IPv4 address. Loopback manager clients authenticate without a token; non-loopback clients require the shared LAN token. Configuration remains under `/etc/kira-service/`; system-service model data resolves to `/var/lib/kira-service/data/kira-service/models/`.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/protocol-v3.md](docs/protocol-v3.md).
