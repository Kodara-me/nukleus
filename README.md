# Kira Service

The **Inpaint** category (`inpaint`, stored under `models/inpaint/`) uses the
same recommended generation parameters, primary weight formats, model types
and optional encoder/VAE files as Image generation. Manage it through the
Manager or SDK installation/configuration APIs. Existing Image generation,
PhotoMaker and IP-Adapter models retain their categories and functionality.
The additive `image.inpaint` operation uploads a PNG source followed by its PNG
mask and runs sd-cli with `--init-img`, `--mask` and `--strength`. It uses the
shared execution FIFO, progress and diagnostic errors. The requested dimensions
must match both images (64–4096 per side); the combined upload fits the configured
image-byte limit. The default denoising strength is 0.99 when omitted. Saved
recommendations are not silently applied by the service. Canvas explicitly
selects the default model's recommendations and recomposes the output through
the mask to preserve unselected pixels. Inpainting also accepts existing
IP-Adapter conditioning fields. Request the reference as a third
`inputRole: ip-adapter` upload after source and mask; its dimensions may differ.
Resolve the opaque CLIP Vision/IP-Adapter model IDs and pass the reference through
`--clip_vision`, `--ip-adapter`, `--ip-adapter-image` and
`--ip-adapter-strength`. Canvas uses the accepted package's first PNG and 0.8.

PhotoMaker is an alternative inpainting conditioning mode. Use its model ID,
packaged embeddings and every packaged portrait via SDK `submitInpaint`;
PhotoMaker and IP-Adapter inputs cannot be combined. Upload source and mask first,
then the ordered references and opaque embeddings. The Service stages identity
files separately and passes `--photo-maker`, `--pm-id-images-dir`,
`--pm-id-embed-path` and `--pm-style-strength`. Canvas's PhotoMaker button uses
style strength 10 and a class word followed by `img`. Reuse requires no extraction.

The **Face parsing** category (`face-parsing`, stored under
`models/face-parsing/`) accepts one `.onnx` file and no companions. The Manager
supports installation, configuration and deletion through the existing atomic
workflows. Mask recommendations default to face regions plus hair, expansion
and soft edges of 8 source pixels, no inversion, preserved aspect ratio, source
output resolution and `CPUExecutionProvider`. These are saved recommendations
only until a job is explicitly submitted. Face parsing runs through the optional
server-side Python/ONNX Runtime worker described below.
Input resolution is not a user setting. Install the desired regular or quantized
ONNX file; there is no quantization toggle or automatic weight conversion.

To enable mask generation, install `numpy`, `Pillow` and `onnxruntime` in a
service-readable Python environment and set `KIRA_FACE_PARSING_PYTHON` to its
Python executable. Under systemd, add that variable to
`/etc/kira-service/kira-service.conf` and restart the service; `ProtectHome`
requires an environment outside home directories, such as `/opt/kira-face-parsing`.
The additive `image.face-parsing` operation accepts the installed model's opaque
ID and source image bytes. It snapshots saved recommendations (shared defaults
when absent), runs on CPU in the shared FIFO, and returns a grayscale PNG.
White selects the included regions; black excludes them. No matching regions,
unreadable images, missing runtime dependencies, incompatible models and timeouts
produce explicit job errors. There is no automatic download or inpainting.

[jonathandinu/face-parsing](https://huggingface.co/jonathandinu/face-parsing)
provides SegFormer/MiT-B5 with 19 CelebAMask-HQ classes and
[regular and quantized ONNX files](https://huggingface.co/jonathandinu/face-parsing/tree/main/onnx).
Use its actual labels (`l_eye`, `l_brow`, `u_lip`, etc.), as documented in
the protocol. This model's card describes non-commercial research/educational
use. It is a reference model, not downloaded or bundled by Kira.

PhotoMaker identity packages support one to four ordered PNG references plus
optional face/hair descriptions and seed metadata. Extraction and generation use
all references; metadata prompts/seed are not applied to generation. Existing
single-reference packages remain supported. Deploy the updated service before
clients that submit multiple references.

Kira Service is the standalone backend for Kira applications. It owns installed models and their recommended options, single HTTPS model installations, the `sd-cli` image queue, and the versioned WebSocket API. Client applications select opaque installed-model IDs and submit jobs; they do not manage service files.

The Manager also supports a **Prompt Enhancer** catalogue (`prompt-enhancer`),
stored under `models/prompt-enhancer/`. Choose subtype **T5**, **LLM** (initial),
or **VLM** to configure the relevant recommended options. Changing subtype resets
its options to the subtype defaults. T5 sampling controls appear only when
sampling is enabled. Primary `.gguf`, `.safetensors`, and `.ckpt` files are accepted;
companions are not supported in this slice. Installation, configuration and
deletion use the existing atomic workflows. `prompt.enhance` executes LLM GGUF
models using a fresh `llama-completion` process per job, without `llama-server`.
T5 and VLM execution are explicitly rejected for now; see
[the option contract](docs/protocol-v3.md).

Install llama.cpp's `llama-completion` executable on the service host to enable
this capability. It must be on the daemon's PATH; alternatively set
`KIRA_LLAMA_COMPLETION` to its absolute path in the service environment.

Model installation preserves existing files: filename collisions receive a
numeric suffix before the extension. Manager's add and configuration forms
accept an optional display name. Model labels use that name when set, otherwise
the full installed filename; opaque IDs remain unchanged when renaming a label.
CLI add/attach also accepts `--display-name "My model"`.

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
including PhotoMaker and IP-Adapter. Editing and upscaling do not use styles.

Add model opens a landscape modal with HTTPS model/companion URLs on the left
and category-specific recommendations on the right, without scrolling.
Progress stays in that modal; success closes it and selects
the installed model. Closing during download cancels the installation and
cleans its temporary files. Only one installation is accepted across clients;
there is no download queue. The image execution queue is unchanged.
The manager connects to `127.0.0.1:9384` and retries while the service is offline.

Recommended options are metadata returned to clients, not enforced execution
settings. Image generation/Edition expose square resolution (256–2048 in steps
of 256), steps, CFG scale, sampler, scheduler, and Clip Skip. LLM exposes
temperature, top-p, context length, and maximum tokens; Upscaler exposes tile
size (execution remains fixed at 2×); IP-Adapter exposes strength; CLIP Vision exposes image size and input normalization. Existing
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
kira-service-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler
kira-service-manager add https://example.com/clip_vision_h.safetensors --category clip_vision
kira-service-manager add https://huggingface.co/h94/IP-Adapter/resolve/main/models/ip-adapter_sd15.safetensors --category ip_adapter
kira-service-manager add https://example.com/model.gguf --category llm-chat --mmproj https://example.com/mmproj.gguf
kira-service-manager add https://example.com/edit.safetensors --category edition --vae https://example.com/vae.safetensors
kira-service-manager attach image-generation:existing.gguf --model-type diffusion --clip-l https://example.com/clip_l.safetensors --clip-g https://example.com/clip_g.safetensors --vae https://example.com/vae.safetensors
kira-service-manager delete upscaler:model.gguf
```

Use `--help` for all options. CLI and GUI manager connections are local-only and do not require manual host or token configuration.

## Models

The `edition` category supports installing, listing, and deleting image-editing models in the manager GUI and CLI (`add <url> --category edition`). It accepts `.safetensors`, `.ckpt`, and `.gguf` files. Editing jobs use image.edit with uploaded reference images.

Service models live in `$XDG_DATA_HOME/kira-service/models/`: image-generation files in `models/`, edition models for image editing in `models/edition/`, upscalers in `models/upscale/`, LLM chat files in `models/llm/`, IP-Adapters in `models/ip-adapter/`, and CLIP Vision encoders in `models/clip_vision/`. Optional companion files are kept under each category's private `companions/<primary filename>/` directory and linked through persistent category-local metadata: mmproj for LLM-chat, VAE for Image-generation and Edition, and LoRA for IP-Adapter. The manager downloads and installs the primary and its optional files together; failed or canceled downloads leave no usable pair, and deleting the primary also removes its companion. Model listings expose `companionFileName` and `companionRole` (`mmproj`, `vae`, or `lora`) without exposing service paths. The manager accepts HTTPS model-file URLs and never overwrites an installed filename. SHA-256 fields and download verification are no longer supported. Primary IP-Adapter and CLIP Vision files support `.bin`, `.safetensors`, `.ckpt`, and `.gguf`; mmproj files require `.gguf`, while VAE and LoRA files accept `.safetensors`, `.ckpt`, or `.gguf`. Query-bearing URLs are accepted and Hugging Face `blob` links are normalized to `resolve` URLs. The add-model modal shows the optional companion URL field appropriate to the selected category. Image-generation CLIP-L, CLIP-G, VAE, LLM text encoder and mmproj companions are passed to sd-cli; LLM-chat mmproj remains a catalog attachment. An explicitly selected IP-Adapter uses its optional auxiliary LoRA through --lora-model-dir and a generated prompt tag. Legacy single-companion installations remain supported. New companionFiles metadata maps roles to category-relative filenames, and modelType defaults to checkpoint when absent.

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

Image-generation and Edition models accept optional `companionUrls` roles `llm`
(text encoder) and `mmproj` (vision projector). Manager add/optional-file dialogs
show both URLs for these categories; CLI add/attach accepts `--text-encoder` and
`--mmproj` together with the existing VAE/CLIP options. LLM-chat `--mmproj` keeps
its legacy meaning. mmproj must be GGUF; text encoders accept GGUF, safetensors,
or ckpt. Files are attached to their primary model, staged and published atomically,
preserved across restart, and deleted with the primary. Generation resolves them
internally and passes `--llm` and `--llm_vision` only when present. Edition jobs use image.edit with uploaded reference-image bytes.

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
and source image bytes. The daemon resolves an Edition model and its optional
LLM, VAE and mmproj companions, queues the uploaded reference, and runs sd-cli
with `--ref-image`, `--prompt`, requested width/height and generation parameters.
Companions use `--llm`, `--vae` and `--llm_vision`; primary weights use
`--diffusion-model` or `--model` according to saved modelType. Reference presets
are auto-detected by sd-cli; no img2img `--init-img` or strength is supplied.
See [upstream editing documentation](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/edit.md).
Canvas selects source dimensions and saved edition recommendations, retaining
source history and normalizing returned output to source resolution if needed.

Reference-image editing also supplies `--flow-shift 3 --offload-to-cpu --diffusion-fa --model-args qwen_image_zero_cond_t=true`. Fatal/error-level Vulkan `ErrorOutOfDeviceMemory`/`ErrorOutOfHostMemory` output aborts the active generation process; the daemon waits for process termination and returns the command and memory diagnostic as a job error, releasing the queue. Warnings, including pinned-memory allocation failures that fall back to CPU buffers, are not treated as failures.

Image generation and reference-image editing both pass `--offload-to-cpu` to sd-cli.

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


## IP-Adapter

The Manager lists `clip_vision` encoders and `ip_adapter` weights and saves their
category options. IP-Adapter keeps its existing `models/ip-adapter/` storage and
opaque IDs; `ip-adapter` remains an accepted category alias. An optional auxiliary
LoRA uses the existing companion workflow.

SDK generation/editing requests can select both installed IDs, supply a distinct
`ipAdapterImageData`, and set `ipAdapterStrength`. The service supplies sd-cli's
`--clip_vision`, `--ip-adapter`, `--ip-adapter-image`, and
`--ip-adapter-strength` arguments. Editing source bytes remain separate.
Recommendations do not activate conditioning automatically; CLIP Vision's
`image_size`/`normalize_input` remain metadata because sd-cli owns preprocessing.
Use compatible classic/Plus SD 1.5 or SDXL weights; other base architectures are
subject to sd-cli support. See [protocol details](docs/protocol-v3.md#ip-adapter-conditioning).

## PhotoMaker V2 identity export

The Manager supports the separate `photomaker` category: one required
`.safetensors` file, stored in `models/photomaker/`, with integer
`style_strength` recommendations (0–100, default 20). Use weights converted for
sd-cli, such as `photomaker-v2.safetensors`; the original .bin weights are not
supported by sd-cli. Installation, configuration and deletion use the normal
model workflow.

The SDK's `exportPhotoMakerIdentity(sourceImageData)` accepts a full image or
an already prepared crop. Its dedicated `photoMakerIdentityExportFinished`
signal returns `PhotoMakerIdentityResult.identityData`: the unchanged
`id_embeds.bin` bytes, never an image. The service runs upstream
`face_detect.py` with a strict single-face guard, reports absent/ambiguous faces
and extraction failures, and removes job files on completion or disconnection.
No PhotoMaker model selection is required for this extraction.

This optional service-host runtime requires a Python environment, the official
script and its InsightFace dependencies/models. Set `KIRA_PHOTOMAKER_PYTHON`
and `KIRA_PHOTOMAKER_FACE_DETECT` as described in
[the host setup guide](docs/photomaker-host-setup.md).
No Python is required in Canvas. The SDK also supports PhotoMaker V2 generation
through `submitGeneration`: select a compatible SDXL `modelId` and
`photoMakerModelId`, supply `photoMakerIdentityData` and the corresponding
`photoMakerReferenceImageData`, and optionally set `photoMakerStyleStrength`
(0–100, default 20). The prompt needs a class word followed by `img`, for example
`a woman img, in a garden`. Both the reference image and the reusable embeddings
are required by sd-cli; renaming the local `.bin` is harmless. The service uploads
both inputs, queues generation and returns a normal image result. Reusing existing
embeddings does not require the extraction Python environment. PhotoMaker cannot
be combined with IP-Adapter or unmasked image-to-image input. Masked PhotoMaker
inpainting uses the separate image.inpaint operation. Canvas generation
integration remains separate work. See the [protocol](docs/protocol-v3.md#photomaker-v2-generation).
See [upstream PhotoMaker documentation](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/photo_maker.md).

## PhotoMaker identity files

The SDK accepts a single `.kiraid` file as `photoMakerIdentityPackageData` for
PhotoMaker generation. It validates and unpacks the compressed ZIP in memory,
then uploads its reference and embeddings through the existing protocol-v3 roles.
`packPhotoMakerIdentity` builds this file from a PNG crop and extracted embeddings.
The archive contains `reference.png`, `id_embeds.bin` and `manifest.json`
(`type: photomaker-v2`, `version: 1`). Legacy paired SDK inputs remain supported;
combining them with a package is rejected. The SDK now requires LibArchive.
