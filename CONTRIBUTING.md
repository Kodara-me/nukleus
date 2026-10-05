# Contributing to Kira Service

## Inpainting

Inpainting uses the existing sd-cli backend; no additional Python dependencies
are required. Update/restart the Service before running an updated Canvas so it
advertises `image.inpaint`. Install/select an Inpaint model through the Manager
and Canvas Service Preferences, along with a default Face parsing model.
Select an image and click the single Inpaint button. Choose a .kiraid and accept
its existing confirmation dialog. Canvas snapshots the source and accepted
descriptions/seed, generates the mask internally, then submits inpainting.
Cancel starts no job. Only the final image is displayed in history. Select saved
default CLIP Vision and IP-Adapter models as well. Canvas uploads the first
accepted package portrait as an independent third image, with IP-Adapter strength
0.8. Use a compatible encoder/adapter pair for the installed SDXL inpaint model.

The separate **Inpaint PhotoMaker** button follows the same two-stage workflow,
using the saved default PhotoMaker model instead of CLIP Vision/IP-Adapter.
It uploads all package portraits and embeddings, stages them under a private
`identity/` job directory, and sets PhotoMaker style strength to 10.
The source image and mask stay outside that directory. This uses existing
embeddings and does not invoke the extraction Python environment.

Canvas submits source-resolution PNGs, the default Inpaint model's recommended
Steps/CFG/sampler/scheduler/Clip Skip values and strength 0.99. It composes any
selected style prefixes locally. Service validates the upload dimensions, stages
source, mask and adapter reference separately, and invokes sd-cli with `--init-img`,
`--mask`, `--clip_vision`, `--ip-adapter`, `--ip-adapter-image` and
`--ip-adapter-strength`. The reference is not required to match source dimensions.
The normal shared FIFO and progress/errors apply. Canvas blends the returned
image through the original grayscale mask and adds a new history entry.

The Service/SDK test uses a disposable sd-cli process to check staged uploads,
arguments, dimension rejection, failures and recovery. Canvas tests exercise
the toolbar, package choice/confirmation, two-stage snapshots, busy guard,
recommendation/seed submission, intermediate-mask suppression,
resolution normalization and exact black-mask pixel preservation. Production
weight quality and runtime compatibility require an installed model; these
fixtures do not claim to validate model inference.

## Face parsing worker

Mask generation has optional service-side Python dependencies. For a local
developer daemon:

```sh
python3 -m venv /tmp/kira-face-parsing-venv
/tmp/kira-face-parsing-venv/bin/python -m pip install numpy Pillow onnxruntime
export KIRA_FACE_PARSING_PYTHON=/tmp/kira-face-parsing-venv/bin/python
./build/src/kira-service
```

Install a SegFormer/CelebAMask-HQ ONNX model in the Face parsing category through
the Manager, then select it as Canvas's default. No PyTorch is required. The
embedded worker takes an RGB image, preserves aspect ratio with padding when
requested, uses the model's NCHW input dimensions (512 when dynamic), applies
the reference model's normalization, resizes logits before selecting semantic
regions, removes padding, then expands/softens the mask in source pixels.
Source dimensions are bounded to 4096 per side. All configured matching regions
are included; this slice does not select one person in a multi-face image.

For systemd, use a persistent environment such as `/opt/kira-face-parsing/venv`,
ensure the service user can read it, and set `KIRA_FACE_PARSING_PYTHON` in
`/etc/kira-service/kira-service.conf`. Restart the daemon after changing it.
The capability is advertised when the configured executable exists; missing
Python packages are reported as job failures. Keep the worker out of protected
home directories. Builds do not install Python packages.

The daemon/SDK tests use a disposable process fixture to exercise upload,
recommendation snapshots, binary delivery, failure, timeout and queue recovery.
Real ONNX inference and mask processing use small generated graphs:

```sh
/tmp/kira-face-parsing-venv/bin/python -m pip install onnx
/tmp/kira-face-parsing-venv/bin/python tests/FaceParsingTest.py
```

CTest registers the Python test when Python is available and skips it if its
optional dependencies are missing. Configure `Python3_EXECUTABLE` with the
environment above to run it through CTest. These tests verify implementation
behavior; visual segmentation quality still depends on the installed weights.

PhotoMaker package tests cover four ordered references and optional face/hair/seed
metadata, plus legacy single-reference archives. SDK and daemon fixtures verify
each upload, all PNGs staged for extraction and sd-cli, and metadata remaining
independent of submitted generation prompts and seed. Multi-reference clients
require the updated service's additive protocol-v3 referenceCount support.

Model labels come from optional server-saved `displayName`, falling back to the
full installed filename. New installations choose an unused filename (numeric
suffix on collision), keeping atomic no-overwrite publication and separate
companion directories. Changing displayName never changes the model ID or files.

Prompt Enhancer models use the `prompt-enhancer` category and managed
`models/prompt-enhancer/` storage. The shared Manager form configures subtype
T5/LLM/VLM recommendations; subtype changes reset defaults. Protocol validation,
catalogue persistence and Manager UI tests cover configuration. LLM GGUF inference
uses the optional `llama-completion` executable from llama.cpp; set
`KIRA_LLAMA_COMPLETION` to override its executable path. No link-time dependency
is added. T5 and VLM inference remain unsupported. Service integration tests use
a subprocess fixture to validate SDK results, parameters, failures and timeout
recovery; they do not require model weights or GPU inference.

Kira Service is a C++20 Qt 6 project. Build dependencies are CMake, Ninja, a C++20 compiler, and Qt 6.2+ Network, WebSockets, and Widgets, SVG, and LinguistTools development packages, plus Kira UI Framework 0.2. `sd-cli` is required on a machine that performs image work.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the daemon with `./build/src/kira-service`. Run the administrator UI with `./build/src/kira-service-manager --gui`, or exercise the manager CLI with `./build/src/kira-service-manager --help`. Use the root installer only for Linux systemd deployments.

Manager GUI connection settings are local QSettings keys `Service/Host` and
`Service/Token`. On startup it tries those settings (loopback/empty key by default)
and opens ServiceConnectionDialog on connection/authentication failure or a
10-second timeout. IPv4/localhost input constructs ws:// on port 9384; credentials
are saved only after authentication. Cancel restores the previous configuration;
background reconnects do not reopen the prompt. CLI does not load GUI settings.
Local Browse streaming is unchanged and cannot import from a remote Manager.
ManagerUiTest covers invalid hosts, failed attempts, authentication, persistence,
restored settings, Cancel and startup prompting with disposable WebSocket fixtures.

Image-editing models use the `edition` catalog category and `models/edition/` storage; the manager GUI, CLI, and SDK support their installation, listing, and deletion. Edition jobs use `image.edit` with uploaded source images.

Sources are organized under `src/daemon/`, `src/manager/`, and `src/sdk/`.
The two executable targets remain declared by `src/CMakeLists.txt`, so their
build paths remain under `build/src/`.

To install only the built executables into a CMake prefix, run
`cmake --install build --prefix "$HOME/.local"`. Use
`scripts/install-kira-service.sh` when a complete Linux systemd installation
is required.

The SDK targets are `KiraService::Protocol` and `KiraService::SDK`. A source
checkout can add `src/sdk` directly, while installed consumers use
`find_package(KiraServiceSdk CONFIG REQUIRED)`. The SDK owns WebSocket and
protocol behavior; applications own settings, files, and UI messages. Keep
protocol v3 compatible. Model management includes the `ip_adapter` category (legacy `ip-adapter` alias) and `clip_vision`
and optional paired companion metadata (IP-Adapter LoRA, LLM mmproj, or Image-generation/Edition VAE).
The SDK install method accepts an optional companion URL instead of a SHA-256
checksum. Download checksums are no longer supported. Image models also accept
`companionUrls` (CLIP-L/CLIP-G/VAE), `modelType` (`checkpoint` or `diffusion`),
and an existing opaque `id` to attach files without downloading the primary again.
The GUI exposes this via Optional files; the CLI exposes `attach` with
`--model-type`, `--clip-l`, `--clip-g`, and `--vae`.

## Shared manager UI

Per-model prompt styles use the framework-independent Protocol `PromptStyle`
value type and JSON validation helpers. The Manager's **Styles…** dialog edits
an in-memory ordered draft for one generation model; Save replaces the list
atomically in category-local metadata and closes only after acknowledgment.
Missing metadata exposes service-owned starters without writing files; explicit
`[]` stays empty. Preserve companion fields and unrelated entries on writes.
Generation resolves `styleId` and snapshots both composed prompts before uploads
and queueing. Clients refresh on `models.styles.changed` and never apply prefixes.
Deploy the service before updated clients. ServiceTest, ServiceSdkTest and
ManagerUiTest use disposable models/network fixtures; do not edit live models.

CMake defaults `KIRA_UI_SOURCE_DIR` to `../kira-ui-framework`. Override it with
`-DKIRA_UI_SOURCE_DIR=/path/to/kira-ui-framework`. Without a source checkout,
provide the installed `KiraUi` package through `CMAKE_PREFIX_PATH`. Only the
manager and its UI tests link `KiraUi::Widgets`; embedded SDK consumers do not
acquire GUI dependencies. Theme and window icons are initialized only in GUI
mode. Original icon artwork and prebuilt PNG sizes are committed under
`assets/icons/`; no image conversion tools are required to build or run.

Manager options use one category-aware form in the details panel and download
modal. Shared defaults and strict wire validation live in the Protocol target's
`ModelOptions` API. Install clients may supply optional `recommendedOptions`;
`configureModel()` replaces a model's complete recommendation object, and
`cancelInstall()` cancels an owned active installation. These APIs do not alter
image job arguments or enable new execution categories.

Category-local `metadata.json` accepts legacy companion strings and new objects
containing `companionFileName`, `companionFiles`, `modelType`, and/or
`recommendedOptions`. Role-specific files live in
`companions/<primary filename>/<role>/<companion filename>`; old single-companion
paths remain valid. Image roles are `clip-l`, `clip-g`, and `vae`. Missing modelType
means a complete checkpoint; choose diffusion explicitly for standalone UNets.
File downloads stage before publication; failures/cancellation preserve an existing
primary and its recommendations, and remove new partial files. Referenced companion
files must be readable and within the primary’s companion directory. Writes preserve
untouched entries and use QSaveFile. Invalid/unreadable JSON blocks mutation
instead of overwriting metadata. Do not edit users' installed files for tests.

The tests create disposable model stores and local HTTP/WebSocket servers.
They cover modular execution flags, companion attachment/persistence/deletion,
download rollback, old metadata, SDK requests and the manager’s optional-file dialog.
The systemd installer test requires Linux (GNU stat/script); on macOS run the full
suite to identify platform limits, then use
`ctest --test-dir build --output-on-failure -E '^kira-service-installer-test$'`
for the portable checks.
Run them with local networking permitted. The unavailable-manager CLI check is
skipped if a real service already responds on port 9384. UI tests isolate
QSettings and use the offscreen platform. For visual review:

```sh
mkdir -p /tmp/kira-manager-review
QT_QPA_PLATFORM=offscreen KIRA_MANAGER_SCREENSHOT_DIR=/tmp/kira-manager-review ./build/tests/kira-service-manager-ui-test
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 KIRA_MANAGER_SCREENSHOT_DIR=/tmp/kira-manager-review ./build/tests/kira-service-manager-ui-test
```

Validate the fixed 1100 × 640 content size (excluding the native title bar),
confirm the window cannot be resized, and review the add-model modal. Qt-only
manager UI code follows the framework's cross-platform behavior; the existing
service deployment, CLI and model-file commit operations retain their platform
requirements. Check available platforms and report any untested ones.

Manager windows and dialogs use native platform title bars and buttons while
keeping the Kira content theme. Window movement/resizing is handled by the OS;
verify actual decorations on each target desktop in addition to offscreen tests.

Generation option validation and backend arguments are covered by a disposable
POSIX fake sd-cli fixture: no installed model or GPU is used. Tests cover automatic
and explicit sampler/scheduler, CFG precision, malformed/out-of-range options,
prompt-only requests with default dimensions/steps/CFG and no companions,
VAE arguments for direct and uploaded-input jobs, and result
bytes. Rebuild and rerun the Linux installer to update a systemd-installed daemon;
a workspace build alone does not update the running service.

Model classification uses a top-level `explicitContent` boolean, separate from
recommendations. SDK and Manager install/configure methods accept an optional
final boolean; omission preserves existing classification. The shared Manager
form edits it alongside recommendations. Validate default false, edits, restart
persistence, list/completion responses, invalid boolean rejection, and legacy
metadata preservation using disposable test models.

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

## Updating a remote Linux installation

Use `scripts/update-kira-service.sh` on the service host as the checkout owner.
It runs `git fetch --prune`, `git pull --ff-only`, CMake configure/build, CTest,
and the existing systemd installer through `sudo -n`. Configure the branch's
upstream first and commit or stash tracked changes. Untracked files are retained;
Git refuses a pull that would overwrite them. Dependencies, including Kira UI
Framework, must already be installed or checked out; sibling repositories and
`sd-cli` are not updated.

```sh
ssh user@service-host '/path/to/kira-service/scripts/update-kira-service.sh'
# Custom build directory and framework checkout:
scripts/update-kira-service.sh --build-dir build-release \
  --cmake-arg -DKIRA_UI_SOURCE_DIR=/path/to/kira-ui-framework
```

New build directories use Ninja and Release; existing CMake cache settings remain
in effect. Additional configure arguments use repeated `--cmake-arg ARG` options;
installer options follow `--`. Installation runs only after tests pass. Valid
existing service configuration and model data are preserved by the installer,
and installation restarts the service, interrupting active jobs. No rollback is
performed if installation fails; inspect its error before retrying.

For password-based sudo, allocate a terminal and cache credentials in the same
SSH session before running the updater:

```sh
ssh -t user@service-host 'sudo -v && /path/to/kira-service/scripts/update-kira-service.sh'
```

Unattended runs require suitable passwordless sudo access. Do not run the entire
updater as root. Build/test failures leave the installed service untouched, but
the checkout may already have advanced to the fetched revision.


## IP-Adapter integration

`clip_vision` adds `models/clip_vision/`; `ip_adapter` reuses the existing
`models/ip-adapter/` directory and opaque IDs. The legacy category alias and
strength-only option objects remain accepted. Both categories use the existing
catalogue's download/import, atomic metadata and no-overwrite behavior. Configure
`image_size`/`normalize_input` for vision and `strength` for
adapters through the Manager. They remain recommendations; sd-cli controls image
preprocessing and requests explicitly choose conditioning IDs/strength.

The SDK adds separate IP-Adapter bytes to generation/editing requests. Protocol v3
uses role-labelled sequential binary uploads so an editing source and conditioning
reference cannot overwrite one another. The daemon resolves both weight IDs,
protects them through the pending/queued/active lifecycle and passes the four
IP-Adapter flags to sd-cli. Auxiliary weights use the existing optional `lora`
companion role and sd-cli's LoRA directory/prompt syntax.

Tests use disposable weight files, local servers and a fake sd-cli to validate
catalogue persistence, booleans/numeric ranges, SDK uploads, distinct editing and
conditioning images, CLI argument precision, defaults, pending-model protection,
and invalid uploads/selections. They do not verify GPU inference or real weight
compatibility. See [protocol-v3.md](docs/protocol-v3.md#ip-adapter-conditioning) for
wire fields and backend limitations.

## PhotoMaker V2 extractor setup

Follow [the host setup guide](docs/photomaker-host-setup.md) for the complete
virtual-environment installation, service-account model provisioning, strict
single-face smoke test, systemd configuration and readiness checks.

PhotoMaker catalogues require no additional build dependency. Identity export
optionally invokes the official `scripts/face_detect.py` from the
stable-diffusion.cpp checkout matching the deployed backend. The bundled
`src/daemon/photomaker-export.py` guard runs it via runpy, preserving its
embedding computation and binary serializer, but refuses multiple faces rather
than choosing the first. It uses the official CPU provider.

Provision a dedicated service-side Python environment with the script's imports:
NumPy, PyTorch, diffusers, safetensors, Pillow, InsightFace 0.7.3 and ONNX Runtime
(CPU). InsightFace installation can require Python development headers and a
compiler. The host guide uses /opt/kira-photomaker/venv and a legacy-compatible
NumPy constraint; follow its dependency installation and smoke-test commands.

Keep the official checkout/script and environment readable by the service account
outside /home (systemd ProtectHome remains enabled). Verify extraction with an
image containing exactly one face under that account, and provision/cache the
InsightFace model assets before relying on offline operation. InsightFace uses
the service account's `~/.insightface`; the installed account's home is
`/var/lib/kira-service`, which the existing systemd sandbox permits writing.
Other Python caches must also live within that writable state directory.

Add these settings to the existing `/etc/kira-service/kira-service.conf`, keeping
its listen/port/token values, then restart the service:

```ini
KIRA_PHOTOMAKER_PYTHON=/opt/kira-photomaker/venv/bin/python
KIRA_PHOTOMAKER_FACE_DETECT=/opt/kira-photomaker/face_detect.py
```

For a foreground daemon, export the same environment variables before launch.
The interpreter defaults to python3; the script has no default. The capability
is advertised only when the interpreter resolves and the script exists; missing
dependencies or model assets return an explicit extraction error. No dependencies
are installed automatically by Kira Service. Valid installer configuration is
preserved by the normal update workflow, including these additional variables.

Exports share the existing FIFO and upload limit (64 MiB by default), run with a
ten-minute timeout and clean temporary image/binary files after the process stops.
Tests replace heavyweight inference with Python fixtures while exercising the
actual bundled single-face guard, subprocess, SDK binary result, failure codes
and disconnection cleanup. They require python3 and skip that fixture if absent;
they do not validate real embedding quality. On a provisioned host, verify a
single face, no face, multiple faces and a cropped source against the deployed
official script. Never persist uploaded identities or introduce identity types
beyond this concrete PhotoMaker binary result.

## PhotoMaker identity packages

Install LibArchive development files before configuring the SDK/service:
`libarchive-dev` on Debian/Ubuntu, `libarchive` on Arch, or `brew install libarchive`
on macOS. Installed SDK consumers also require this CMake dependency.
Use `photoMakerIdentityPackageData` with complete `.kiraid` bytes instead of
separate reference/embedding inputs. The SDK validates the version-1 PhotoMaker V2
manifest, exact entry names, duplicates, entry types and bounded decompressed sizes
before any submission. Extraction still returns only the binary embedding;
Canvas packages it together with the exact crop. The daemon's protocol and
temporary-file lifecycle remain unchanged.
