# Kira Service

Kira Service is the standalone backend for Kira applications. It owns installed models and their recommended options, single HTTPS model installations, the `sd-cli` image queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

Editing reuses **Image generation** models directly. image.edit accepts their
original opaque IDs and resolves the same primary weights, model type and companions.
There is no separate editing category or eligibility checkbox. Select a generation
model as Canvas's default edit model. Existing legacy model directories remain on disk.

Prompt enhancement uses the **LLM** catalogue (llm-chat, stored under models/llm/).
The identity dialog in Canvas selects its default LLM model in Service Preferences.
prompt.enhance runs a fresh llama-completion process with the model's temperature,
top-p, context length and maximum tokens. The identity-specific system prompt is
a per-job override. Model settings are unchanged by individual requests.

Install llama.cpp's `llama-completion` executable on the service host to enable
this capability. It must be on the daemon's PATH; alternatively set
`KIRA_LLAMA_COMPLETION` to its absolute path in the service environment.

Model installation preserves existing files: filename collisions receive a
numeric suffix before the extension. Manager's add and configuration forms
require a nonblank display name. Model labels use that name; opaque IDs remain unchanged when renaming a label.
CLI add requires `--display-name "My model"`; an explicit empty rename is rejected.

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

`kira-service` is the daemon. `kira-service-manager --gui` opens the
administrator UI with the shared dark Kira theme: a category/model tree on the
left and editable recommended options on the right. Its window content has a fixed,
non-resizable size of 1100 × 640 logical pixels, excluding the native title bar.
Saved geometry restores the window position; previous sizes and maximized states are ignored.
Select a model or its companion to edit recommendations, then Save to persist
them on the service. The shared add/edit form includes **Explicit content (NSFW)**,
unchecked by default; the service persists the flag and returns it with model listings.
Unsaved edits prompt before leaving the model.

The GUI tries its last successful service connection at startup, defaulting to
`127.0.0.1:9384`. If connection or authentication fails, enter the server's IPv4
address (or `localhost`) and key in the connection dialog. Enter only the host;
the Manager builds `ws://<host>:9384`. A successful authentication saves the
connection for next launch; failed attempts and Cancel do not change saved settings.
Remote servers must have a reachable LAN listener and token configured; use a
trusted LAN or VPN. Browse imports remain local-only. CLI commands still use loopback.

For an image-generation model, **Styles…** edits its ordered list of named
positive/negative prompt prefixes. Save waits for the service acknowledgment;
Cancel discards the draft. Models without saved styles expose Photographic,
Cinematic and Illustration starters; saving an empty list removes them.
Canvas selects a style, and the service applies its prefixes once for generation,
 Editing and upscaling do not use styles.

Add model opens a landscape modal with HTTPS model/companion URLs on the left
and category-specific recommendations on the right, without scrolling.
Progress stays in that modal; success closes it and selects
the installed model. Closing during download cancels the installation and
cleans its temporary files. Only one installation is accepted across clients;
there is no download queue. The image execution queue is unchanged.
The manager connects to `127.0.0.1:9384` and retries while the service is offline.

Recommended options are metadata returned to clients, not enforced execution
settings. Image generation expose square resolution (256–2048 in steps
of 256), steps, CFG scale, sampler, scheduler, and Clip Skip. LLM exposes
temperature, top-p, context length, and maximum tokens; Upscaler has no options or explicit-content control (execution remains fixed at 2×); Existing
models without recommendations remain usable; proposed defaults are saved only
on an explicit Save. Canvas uses the selected model’s recommendations for unedited generation controls.

Image models can be complete checkpoints or standalone diffusion models (for
example, a split UNet GGUF). The add dialog includes **Model contents** plus
optional CLIP-L, CLIP-G, VAE, LLM text encoder and mmproj URLs. Select an installed image model and choose
**Optional files…** to attach missing files without downloading its primary again.
Leave unused URLs empty; complete checkpoints keep working without extra files.
Already attached roles cannot be overwritten. Choose compatible components from
the model publisher; standalone SDXL UNets normally need both CLIP encoders and a VAE.
The service passes only attached files to `sd-cli`: `--clip_l`, `--clip_g`, and
`--vae`. Standalone models use `--diffusion-model`; complete checkpoints use
`--model`. See the [upstream CLI options](https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/common/common.cpp).

The manager also provides local CLI operations:

```sh
kira-service-manager list
kira-service-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler --display-name "Upscaler"
kira-service-manager add https://example.com/projector.gguf --category companion --companion-type mmproj --display-name "Vision projector"
kira-service-manager add https://example.com/model.gguf --category llm-chat --display-name "Chat" --mmproj-model companion:projector.gguf
kira-service-manager add https://example.com/edit.safetensors --category image-generation --display-name "Image model" --vae https://example.com/vae.safetensors
kira-service-manager attach image-generation:existing.gguf --model-type diffusion --clip-l https://example.com/clip_l.safetensors --clip-g https://example.com/clip_g.safetensors --vae https://example.com/vae.safetensors
kira-service-manager delete upscaler:model.gguf
```

Use `--help` for all options. CLI and GUI manager connections are local-only and do not require manual host or token configuration.

## Models

Image-generation models support generation and image.edit with uploaded reference images.

Service models live in `$XDG_DATA_HOME/kira-service/models/`: image-generation files in `models/`, upscalers in `models/upscale/`, LLM chat files in `models/llm/`. Optional companion files are kept under each category's private `companions/<primary filename>/` directory and linked through persistent category-local metadata: VAE for Image-generation. The manager downloads and installs the primary and its optional files together; failed or canceled downloads leave no usable pair, and deleting the primary also removes its companion. Model listings expose `companionFileName` and `companionRole` (`mmproj` or `vae`) without exposing service paths. The manager accepts HTTPS model-file URLs and never overwrites an installed filename. SHA-256 fields and download verification are no longer supported. mmproj files require `.gguf`, while VAE files accept `.safetensors`, `.ckpt`, or `.gguf`. Query-bearing URLs are accepted and Hugging Face `blob` links are normalized to `resolve` URLs. The add-model modal shows the optional companion URL field appropriate to the selected category. Image-generation CLIP-L, CLIP-G, VAE, LLM text encoder and mmproj companions are passed to sd-cli; LLM models reference separately installed MMPROJ companions by ID.  Legacy single-companion installations remain supported. New companionFiles metadata maps roles to category-relative filenames, and modelType defaults to checkpoint when absent.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Linux system service

```sh
sudo scripts/install-kira-service.sh --binary build/src/kira-service --manager-binary build/src/kira-service-manager
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, installs the manager into the invoking user's `~/.local/bin`, and restarts updated installations. A LAN binding listens on both `127.0.0.1` and the selected IPv4 address. Loopback manager clients authenticate without a token; non-loopback clients require the shared LAN token. Configuration remains under `/etc/kira-service/`; system-service model data resolves to `/var/lib/kira-service/data/kira-service/models/`.

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/protocol-v3.md](docs/protocol-v3.md).

Manager windows and dialogs use native platform title bars and buttons while
keeping the Kira content theme. Window movement/resizing is handled by the OS;
verify actual decorations on each target desktop in addition to offscreen tests.

Generation requests remain caller-controlled: recommendedOptions are never applied
implicitly. The daemon validates option types/ranges before queueing, omits
Automatic sampler/scheduler flags and preserves CFG precision. Installed VAE
companions are resolved on the server and passed to both text-to-image and
image-to-image jobs without exposing filesystem paths.
Omitted dimensions and steps default to 1024 × 1024 and 28; CFG defaults to 7.
No external VAE is required for a complete checkpoint with an embedded VAE.
Standalone diffusion files require their missing encoder/VAE weights separately.

Image-generation models accept optional `companionUrls` roles `llm`
(text encoder) and `mmproj` (vision projector). Manager add/optional-file dialogs
show both URLs for these categories; CLI add/attach accepts `--text-encoder` and
`--mmproj` together with the existing VAE/CLIP options. LLM models use --mmproj-model with an installed MMPROJ companion ID instead of a URL. mmproj must be GGUF; text encoders accept GGUF, safetensors,
or ckpt. Files are attached to their primary model, staged and published atomically,
preserved across restart, and deleted with the primary. Generation resolves them
internally and passes `--llm` and `--llm_vision` only when present. Editing jobs use image.edit with uploaded reference-image bytes.

Manager add/optional-file dialogs offer **Browse…** beside the model and companion
sources. Selected local files are copied by the service through the same staged,
atomic installation workflow as downloads; originals are retained and existing
installed files are never overwritten. Installation requests may use `file://`
URLs only from loopback clients. LAN clients remain restricted to HTTPS sources.
The Manager streams selected files from a temporary localhost HTTP source with
an unguessable per-file URL. The service copies these through its existing download
pipeline, so systemd ProtectHome and PrivateTmp remain enabled. Loopback installation
clients may supply HTTP sources on 127.0.0.1; LAN requests cannot. Model metadata stores managed relative filenames,
never the source paths. No Hugging Face token or login is stored or required for
local imports.

Companions can be removed independently through the Manager's **Optional files…**
dialog. Confirm **Remove** beside an attached role, then choose a new URL or
Browse source and apply the replacement. Removal preserves the primary, other
companions, recommendations and explicit-content classification; queued/active
models and models receiving files cannot lose companions.

Protocol v3 adds `models.companion.delete` with opaque model `id` and companion
`role`, returning `models.companion.delete.completed` with both fields or an error.
SDK/Manager clients expose `deleteCompanion(id, role)` and `companionDeleted`.
Replacement uses the existing attachment install operation; it is a separate step,
so a failed replacement leaves the removed role absent. Legacy single-companion
metadata is supported and updated without discarding unrelated metadata.

Generation failures include the attempted sd-cli command (executable and quoted
arguments), exit code and up to 4096 trailing stderr characters in the job error
message. This diagnostic intentionally exposes the submitting client's prompts
and service-side model/output paths to that client only; successful results and
model listings continue to omit service paths. Commands are displayed as POSIX
shell-quoted text for diagnosis; execution still uses QProcess argument lists.

Image editing uses the additive protocol-v3 `image.edit` operation and capability.
The SDK exposes `submitEdit(ImageGenerationRequest)` with nonempty instructions
and source image bytes. The daemon resolves an image-generation model and its optional
LLM, VAE and mmproj companions, queues the uploaded reference, and runs sd-cli
with `--ref-image`, `--prompt`, requested width/height and generation parameters.
Companions use `--llm`, `--vae` and `--llm_vision`; primary weights use
`--diffusion-model` or `--model` according to saved modelType. Reference presets
are auto-detected by sd-cli; no img2img `--init-img` or strength is supplied.
See [upstream editing documentation](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/edit.md).
Canvas selects source dimensions and saved generation recommendations, retaining
source history and normalizing returned output to source resolution if needed.

Reference-image editing also supplies `--flow-shift 3 --offload-to-cpu --diffusion-fa --model-args qwen_image_zero_cond_t=true`. Fatal/error-level Vulkan `ErrorOutOfDeviceMemory`/`ErrorOutOfHostMemory` output aborts the active generation process; the daemon waits for process termination and returns the command and memory diagnostic as a job error, releasing the queue. Warnings, including pinned-memory allocation failures that fall back to CPU buffers, are not treated as failures.

Image generation and reference-image editing both pass `--offload-to-cpu` to sd-cli.
For temporary testing, all image generation and editing calls also
pass `--diffusion-fa` exactly once. Per-model configuration is deferred.

## Remote Linux updates

From another machine, run the updater over SSH on the service host:

```sh
ssh user@service-host '/path/to/kira-service/scripts/update-kira-service.sh'
```

Run as the checkout owner with cached or passwordless sudo credentials. The script
fetches and fast-forwards the current branch's upstream, configures and builds,
runs CTest, then uses the existing systemd installer to preserve configuration and
restart the service. Tracked changes, diverged branches, and build/test failures
stop the update before installation. The restart interrupts active jobs.
Build dependencies and Kira UI Framework must already be available; sibling
checkouts and `sd-cli` are not updated. See `--help` for build and installer options.


## Identity portrait packages

Canvas generates portraits through ordinary image generation and packages the
selected PNGs and face/hair prompts locally. The SDK IdentityPackage helper writes
a compressed .kiraid ZIP containing manifest.json (type kira-identity, version 2),
reference.png and up to three additional ordered reference PNGs, plus seed metadata.
No extraction backend or binary payload is required. Existing user models and
older identity files are retained on disk.

Supported categories are Image generation, Upscaler, LLM and Companion.
Removed model directories and existing identity files are retained on disk.
Rebuild SDK consumers and deploy the updated Service alongside the updated clients.

## Shared companions and model forms

Companion entries are grouped as VAE and MMPROJ (vision projector) in Manager.
Install them with their own required names. VAE supports safetensors, ckpt and
GGUF; MMPROJ supports GGUF. LLM forms replace the optional mmproj URL with a
dropdown of installed MMPROJ entries, plus None. Save persists the selected ID;
clearing it detaches the relationship without deleting the companion.
Assigned companions cannot be deleted or retyped until detached from every LLM.
The current prompt-enhancement operation remains text-only.

Selecting any category or subtype leaves the right panel empty. Selecting a
model shows its required display name and relevant fields. Upscaler has no model
options, explicit-content checkbox or recommendations hint. LLM retains its
sampling/token fields but has no recommendations hint.
