# Nukleus

Nukleus is the standalone backend for Kira applications. It owns installed models and their recommended options, single HTTPS model installations, the `sd-cli` image queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

Editing reuses **Image generation** models directly. image.edit accepts their
original opaque IDs and resolves the same primary weights, model type and companions.
There is no separate editing category. **Can Edit** and **Can Generate** control
which operations each model supports; at least one must be checked. Only models that
can edit use reference images, also when generating: the service rejects references
sent to any other model. Select an
eligible model as Canvas's default edit model. Missing flags preserve both
operations for existing models. Existing legacy model directories remain on disk.

Prompt enhancement uses the **LLM** catalogue (llm-chat, stored under models/llm/).
The identity dialog in Canvas selects its default LLM model in Service Preferences.
prompt.enhance runs on the same loopback `llama-server` as text.chat, with the
model's temperature, top-p, context length and maximum tokens. The model stays
loaded between enhancements and is shared with chats that use text.chat's
default load settings; another model or setting unloads it and loads the new
one, and image jobs unload it first. Without `llama-server`, a fresh
`llama-completion` process runs each request. The identity-specific system
prompt is a per-job override. Model settings are unchanged by individual requests.

Install llama.cpp's `llama-server` (or `llama-completion`) executable on the
service host to enable this capability. It must be on the daemon's PATH;
alternatively set `NUKLEUS_LLAMA_SERVER` or `NUKLEUS_LLAMA_COMPLETION` to its
absolute path in the service environment.

text.chat sends a client-built conversation (system, user and assistant
messages) to an installed LLM and returns one complete reply. It is generic:
the service knows nothing about the client's domain. Its default `server`
engine runs llama.cpp's `llama-server` on loopback and keeps the model loaded
between turns; the `completion` engine reuses `llama-completion` and reloads
the model on every turn. Clients choose the engine, sampling, GPU layer count
and KV-cache placement per request. `llama-server` starts on the first chat
that needs it, or earlier through `text.chat.load`; `text.chat.unload` stops
it. The service also unloads it before any other job and after ten minutes
without a chat. Set `NUKLEUS_LLAMA_SERVER` to
override the executable and `NUKLEUS_LLAMA_SERVER_IDLE_SECONDS` to change the
idle period, for example in `/etc/nukleus/nukleus.conf`. See
[`docs/protocol-v3.md`](docs/protocol-v3.md).

Model installation preserves existing files: filename collisions receive a
numeric suffix before the extension. Manager's add and configuration forms
require a nonblank display name. Model labels use that name; opaque IDs remain unchanged when renaming a label.
CLI add requires `--display-name "My model"`; an explicit empty rename is rejected.

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

`nukleusd` is the daemon. `nukleus-manager --gui` opens the
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

**Presets…** opens a service-wide tree with fixed Generation and Editing branches.
Generation starts with **Photorealism / Photoréalisme** and **Illustration**.
**Add preset** (+) below the tree creates an unassigned preset with default names;
**Remove preset** (×) removes the selected preset. Generation
keeps between 1 and 16 presets; drag/drop is not available. Canvas falls back to
the first preset when its selected preset is removed.
Manager displays preset names in English regardless of its system locale.
Select a preset to edit its English and French names, choose its eligible model, edit **Steps** and **CFG Scale**
and its optional positive/negative prefixes, and choose how many **Identity
portraits** (1, 2 or 4, default 1) Canvas generates with it. **Editing** holds the
fixed **Fast**, **Balanced** and **Quality** qualities offered by Canvas's edit
slider: select one to choose its editing model, Steps/CFG and optional
positive/negative prefixes, and tick **Default quality** to make it the quality Canvas preselects
and uses for edits without a quality choice, such as applying an identity
(Balanced initially; exactly one quality is the default). Selecting a model
snapshots its recommended values (shared defaults when absent); Save persists
that pair, including manual reductions in steps. Catalogue refreshes never
replace saved values. Models can be reused across presets and editing.
Save persists the choices; Cancel discards them.
Presets have no prompt flow: LLM help (translation, prompt improvement) comes from
the **Default prompt translator** and **Default prompt enhancer** LLM checkboxes,
which clients run through `prompt.enhance`.
Mappings start unassigned and require explicit setup before Canvas can submit.
Save waits for acknowledgment; Cancel discards all draft edits. Assigned models
cannot be deleted or lose required capabilities until unassigned.
Version-6 `models/model-routing.json` stores both English and French labels,
each preset's and editing quality's model with Steps/CFG, the default editing
quality, and each preset's identity portrait count atomically; versions 1–4 load with one identity portrait, and
versions 4–5 copy their single editing model into all three qualities. Canvas
chooses the system locale, falling back to English. Versions 1–3 stored
Fast/Balanced/Quality tiers: presets load with their **Fast** (`-low`) tier,
dropping the others, while editing keeps all three tiers and its permanent tier
(if any) becomes the default. Prompt flows saved by earlier versions are ignored.
Missing parameters are initialized from model recommendations, and the file is
rewritten (as version 6) only on explicit Save.
Version-1 custom entries are not migrated. Unreadable/malformed
configuration is never overwritten. Deploy Service before updated Canvas.
Legacy per-model style protocol/metadata remain compatible with older clients,
but their management dialog is removed.

Add model opens a landscape modal with HTTPS model/companion URLs on the left
and category-specific recommendations on the right, without scrolling.
Progress stays in that modal; success closes it and selects
the installed model. Closing during download cancels the installation and
cleans its temporary files. Only one installation is accepted across clients;
there is no download queue. The image execution queue is unchanged.
The manager connects to `127.0.0.1:9384` and retries while the service is offline.

Recommended options are metadata returned to clients, not enforced execution
settings. Image generation expose square resolution (256–2048 in steps
of 256), steps, CFG scale, sampler, scheduler, generation/edit capabilities and the prompt
format clients write for it (natural sentences, short phrases by default, or tags). LLM exposes
temperature, top-p, context length, and maximum tokens; Upscaler has no options or explicit-content control (execution remains fixed at 2×); Existing
models without recommendations remain usable; proposed defaults are saved only
on an explicit Save. Canvas uses the selected model’s recommendations for unedited generation controls.

Image models can be complete checkpoints or standalone diffusion models (for
example, a split UNet GGUF). The add dialog includes **Model contents** plus
dropdowns for an installed **Companion / VAE** and an installed **LLM** marked
**Can be used as text encoder**, both with None. These are shared references;
deleting the image model never deletes their weights. Assigned models cannot be
deleted, retyped or have text encoder eligibility removed until detached.
CLIP-L, CLIP-G, Clip Skip and the image model's optional mmproj controls are removed.
The service passes assignments as `--vae` and `--llm`; the selected LLM's MMPROJ
assignment supplies `--llm_vision` when present. Standalone models use `--diffusion-model`; complete checkpoints use
`--model`. See the [upstream CLI options](https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/common/common.cpp).

The manager also provides local CLI operations:

```sh
nukleus-manager list
nukleus-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler --display-name "Upscaler"
nukleus-manager add https://example.com/projector.gguf --category companion --companion-type mmproj --display-name "Vision projector"
nukleus-manager add https://example.com/model.gguf --category llm-chat --display-name "Chat" --mmproj-model companion:projector.gguf
nukleus-manager add https://example.com/edit.safetensors --category image-generation --display-name "Image model" --vae https://example.com/vae.safetensors
nukleus-manager attach image-generation:existing.gguf --model-type diffusion --clip-l https://example.com/clip_l.safetensors --clip-g https://example.com/clip_g.safetensors --vae https://example.com/vae.safetensors
nukleus-manager delete upscaler:model.gguf
```

Use `--help` for all options. CLI and GUI manager connections are local-only and do not require manual host or token configuration.

## Models

Image-generation models support generation and image.edit with uploaded reference images.

Service models live in `$XDG_DATA_HOME/nukleus/models/`: image-generation files in `models/`, upscalers in `models/upscale/`, LLM files in `models/llm/`, animation (image-to-video) models in `models/animation/`, and shared VAE/MMPROJ/T5-XXL companions in `models/companion/`. Assignments use opaque IDs and never expose service paths or delete shared files with a primary. The manager accepts HTTPS sources and local imports, never overwriting installed weights. VAE accepts `.safetensors`, `.ckpt`, or `.gguf`; MMPROJ requires `.gguf`; T5-XXL accepts `.gguf` or `.safetensors`. Hugging Face `blob` links normalize to `resolve` URLs. SHA-256 verification is not supported. Legacy category-local attachments remain stored under `companions/<primary filename>/`; legacy VAE/LLM weights remain usable as fallbacks. modelType defaults to checkpoint when absent.

`nukleusd --models-dir DIR --config-dir DIR` selects a split layout instead: weights in `models-dir/{generation,upscale,llm,companion,animation}/` and the catalogue (`generation.json`, `upscale.json`, `llm.json`, `companion.json`, `animation.json`, each in the former category `metadata.json` format, plus `model-routing.json`) in `config-dir/catalog/`. Both options are required together; the daemon creates the directories at startup. See `docs/plans/storage-layout.md`.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist.

## Linux system service

### Runtime tools on Arch Linux

The daemon runs `sd-cli` for image jobs and llama.cpp's `llama-server` or
`llama-completion` for text jobs. On Arch Linux and derivatives such as
CachyOS, build both with the Vulkan backend from the AUR:

```sh
paru -S llama.cpp-vulkan-git stable-diffusion.cpp-vulkan-git
```

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

### Installing the service

```sh
sudo scripts/install-nukleus.sh --binary build/src/nukleusd --manager-binary build/src/nukleus-manager
```

The installer uses port `9384`, preserves a valid existing bind/token configuration, installs the manager into the invoking user's `~/.local/bin` with an application-menu entry (`~/.local/share/applications`, icons under `~/.local/share/icons/hicolor` referenced by absolute path); `--desktop-shortcut` also adds a shortcut to the user's XDG desktop directory, or `--desktop-dir PATH` to another directory, and restarts updated installations. A LAN binding listens on both `127.0.0.1` and the selected IPv4 address. Loopback manager clients authenticate without a token; non-loopback clients require the shared LAN token. Configuration remains under `/etc/nukleus/`; system-service model data resolves to `/var/lib/nukleus/data/nukleus/models/`.

To use the split layout, pass `--models-dir DIR --config-dir DIR` to the installer (or after `--` to the updater). The installer stores both in `nukleus.conf`, keeps them on later updates, creates the category and `catalog/` directories, and restricts the unit's `ReadWritePaths=` to them. When a storage directory belongs to a non-root group (for example a shared `/srv/lab` owned by `users`), the service joins that group instead of taking ownership. Copy an existing model tree first with:

```bash
scripts/migrate-nukleus-storage.sh --source OLD_MODELS --models-dir /srv/lab/models --config-dir /srv/lab/config/nukleus --dry-run
```

Uninstalling, even with `--purge`, never removes the split storage directories.

### Upgrading from Kira Service

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

See [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/protocol-v3.md](docs/protocol-v3.md).

Manager windows and dialogs use native platform title bars and buttons while
keeping the Kira content theme. Window movement/resizing is handled by the OS;
verify actual decorations on each target desktop in addition to offscreen tests.

Generation requests remain caller-controlled: recommendedOptions are never applied
implicitly, except for `image.animate`, which uses an animation model's defaults for the
options a request omits (see [Animating an image](#animating-an-image)). The daemon validates option types/ranges before queueing, omits
Automatic sampler/scheduler flags and preserves CFG precision. Installed VAE
companions are resolved on the server and passed to both text-to-image and
image-to-image jobs without exposing filesystem paths.
Omitted dimensions and steps default to 1024 × 1024 and 28; CFG defaults to 7.
No external VAE is required for a complete checkpoint with an embedded VAE.
Standalone diffusion files require their missing encoder/VAE weights separately.

Image models save `canEdit`, `canGenerate`, `vaeModelId` and `textEncoderModelId`
inside recommendedOptions; LLM saves `canBeTextEncoder` (default false) there.
LLM forms also offer **Default prompt enhancer**, **Default prompt translator** and
**Default image describer** (`defaultPromptEnhancer`/`defaultPromptTranslator`/
`defaultImageDescriber`): at most one LLM holds each role, possibly the same one,
and saving a checked role moves it to that model. An LLM with an **MMPROJ
companion** can see images: text chats may attach up to eight PNG or JPEG images.
The service validates references and capabilities before accepting jobs. Legacy
attachment APIs/CLI remain compatible and existing files remain on disk; legacy
VAE/LLM files are fallback weights when no shared assignment is selected. Legacy
CLIP and image-level mmproj attachments are no longer passed to sd-cli.
**Optional files…** retains the legacy VAE removal/replacement workflow.
Editing jobs use image.edit with uploaded reference-image bytes.

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

Reference-image editing also supplies `--flow-shift 3 --offload-to-cpu --diffusion-fa --vae-conv-direct --model-args qwen_image_zero_cond_t=true`. Fatal/error-level Vulkan `ErrorOutOfDeviceMemory`/`ErrorOutOfHostMemory` output aborts the active generation process; the daemon waits for process termination and returns the command and memory diagnostic as a job error, releasing the queue. Warnings, including pinned-memory allocation failures that fall back to CPU buffers, are not treated as failures.

Image generation and reference-image editing both pass `--offload-to-cpu` to sd-cli.
For temporary testing, all image generation and editing calls also
pass `--diffusion-fa` exactly once. They also pass `--vae-conv-direct`, which
decodes with direct convolutions instead of oversized im2col buffers that exceed
the Vulkan pinned-memory limit. Per-model configuration is deferred.

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


## Identity portrait packages

Canvas generates portraits through ordinary image generation and packages the
selected PNGs and face/hair prompts locally. The SDK IdentityPackage helper writes
a compressed .kiraid ZIP containing manifest.json (type kira-identity, version 2),
reference.png and up to three additional ordered reference PNGs, plus seed metadata
and generation/edit prompts describing how to use the references.
No extraction backend or binary payload is required. Existing user models and
older identity files are retained on disk.

Supported categories are Image generation, Upscaler, LLM, Companion and Animation.

## Animation models

The Animation category (`animation`) keeps image-to-video models such as Wan 2.2
I2V with every file they need. Its primary file is the low-noise diffusion
model; the high-noise diffusion model and a speed-up LoRA for each stage are
optional attachments (`high-noise`, `lora-low-noise`, `lora-high-noise`), and the
VAE and the T5-XXL text encoder are shared companions selected by ID
(`vaeModelId`, `textEncoderModelId`, both required). One `models.install`
downloads the model and its attachments; optional files can be attached or
removed later without downloading the model again. `models.list` reports the
roles whose file or companion is absent in `missingFiles`; see
[the protocol](docs/protocol-v3.md#animation-models).

In the Manager, the Animation branch lists each model with its attached files
and its VAE and text encoder, and marks every missing one. The add form takes
the diffusion model, the optional high-noise model and LoRAs (URL or
**Browse…**), and for the VAE and the text encoder either an installed companion
of the right subtype or **New from URL or file…**. On **Start download**, the
Manager installs each new companion first (named after its file), then the model
with their IDs, one installation at a time. Closing the form cancels the running
installation; companions already installed stay in the catalogue and can be
selected again. **Optional files…** attaches or removes the high-noise model and
the LoRAs of an installed model, and attaches again a file reported missing.

The options panel of an animation model also edits its default video options,
grouped as Video, Sampling, LoRA, Cache, Memory and VAE tiling, with each
high-noise option beside its low-noise counterpart. Every field may be left
empty: an empty field means no model default, so a job falls back to sd-cli's
own default for it. Save writes the filled fields only, with the VAE and text
encoder IDs; a value outside its range shows a message and blocks Save.

```sh
nukleus-manager add https://example.com/umt5-xxl-encoder-Q8_0.gguf --category companion --companion-type t5xxl --display-name "UMT5-XXL"
nukleus-manager add https://example.com/wan2.2_i2v_low_noise_14B_Q4_K_M.gguf --category animation --display-name "Wan 2.2 I2V" \
    --vae-model companion:wan_2.1_vae.safetensors --text-encoder-model companion:umt5-xxl-encoder-Q8_0.gguf \
    --high-noise https://example.com/wan2.2_i2v_high_noise_14B_Q4_K_M.gguf \
    --lora-low-noise https://example.com/low_noise_lightx2v_4step.safetensors \
    --lora-high-noise https://example.com/high_noise_lightx2v_4step.safetensors
nukleus-manager attach animation:wan2.2_i2v_low_noise_14B_Q4_K_M.gguf --lora-high-noise https://example.com/high_noise_lightx2v_4step.safetensors
nukleus-manager attach animation:wan2.2_i2v_low_noise_14B_Q4_K_M.gguf --vae-model companion:other_vae.safetensors
```

`--vae-model` and `--text-encoder-model` are required for `add`; on `attach`,
either one replaces that companion and keeps the other.

Removed model directories and existing identity files are retained on disk.
Rebuild SDK consumers and deploy the updated Service alongside the updated clients.

### Animating an image

`image.animate` turns a PNG or JPEG start image, and optionally an end image,
into a short video with an installed animation model, through
`sd-cli -M vid_gen`. A request carries the model ID, a prompt, an optional
negative prompt and seed, and a `parameters` object holding any sd-cli video
option of the [option table](docs/protocol-v3.md#animating-an-image-imageanimate):
size, frame count, fps, container (`webm`, `webp`, `avi`), steps, guidance and
sampler for each noise pass, flow shift, LoRA weights, cache, memory and VAE
tiling options. Each option the request leaves out comes from the model's
`recommendedOptions`, else is left to sd-cli's own default; this is the one
operation that applies recommendations to jobs. The model's LoRAs are added to
the prompt by the service with their stage and weight (`loraWeights`, default 1,
0 leaves one out). Jobs share the FIFO with image jobs, report the same
`progress` messages with the sampling step counted across both noise passes,
and stop when their client disconnects. The result is the video bytes with its
MIME type, frame count and fps. SDK clients use `canAnimate()`,
`submitAnimation(AnimationRequest)` and `animationFinished(AnimationResult)`.
Ping-pong loops and MP4 output are not available yet and fail with
`backend-not-ready`.

## Shared companions and model forms

Companion entries are grouped as VAE, MMPROJ (vision projector) and T5-XXL
(animation text encoder) in Manager.
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

## License

Nukleus is released under the [MIT License](LICENSE).
