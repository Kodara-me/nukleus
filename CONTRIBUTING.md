# Contributing to Kira Service

Every new install and explicit rename requires nonblank server-saved displayName
(1–256 trimmed characters). Missing new names and empty renames are rejected. New installations choose an unused filename (numeric
suffix on collision), keeping atomic no-overwrite publication and separate
companion directories. Changing displayName never changes the model ID or files.

Prompt enhancement executes an installed LLM GGUF model through the optional
llama-completion executable; set KIRA_LLAMA_COMPLETION to override its path.
It uses standard LLM recommendations (temperature, topP, contextLength, maxTokens)
and permits a per-job systemPrompt. Tests cover overrides, failures, timeouts and
disconnect recovery with a subprocess fixture, without model weights or a GPU.
No Python worker or optional Python dependency is required.

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

Image editing uses image-generation models and their existing weights/companions through image.edit. The same generation IDs work for both operations, without an eligibility flag or a separate model category.

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
protocol v3 compatible. Model management preserves optional companion metadata
for Image-generation VAE. LLM MMPROJ files are separately installed Companion entries.
The SDK install method accepts an optional companion URL instead of a SHA-256
checksum. Download checksums are no longer supported. Image models also accept
`companionUrls` (CLIP-L/CLIP-G/VAE), `modelType` (`checkpoint` or `diffusion`),
and an existing opaque `id` to attach files without downloading the primary again.
The legacy VAE GUI exposes this via Optional files; the compatibility CLI exposes `attach` with
`--model-type`, `--clip-l`, `--clip-g`, and `--vae`.

## Shared manager UI

ModelRouting Protocol types carry English/French labels, preset prefixes
and one ModelAssignment (model plus optional Steps/CFG snapshot) per preset and
per fixed editing quality (Fast/Balanced/Quality, exactly one the default). Use the shared KiraUi TreeView with fixed Generation/Editing
branches: 1–16 presets under Generation (starting with Photorealism and
Illustration); the three editing qualities under
Editing, each with its own prefixes. Add/Remove
preset buttons below the tree create a preset with a generated `preset-…` ID or
remove the selected one; the tree cannot drag, reorder or reparent entries.
Selecting a preset shows its English/French names, Model/Steps/CFG and prefixes;
selecting an editing quality shows Model/Steps/CFG, its prefixes and its Default
quality checkbox. Model changes initialize the pair
from recommendations or shared defaults; catalogue refresh preserves it. Save
persists manual values; Cancel discards them.

Store version-6 JSON atomically beside model metadata; each preset's
`identityViews` (1, 2 or 4) defaults to 1 for versions 1–4, and versions 4–5
copy their single `editing` assignment into every editing quality. Versions 1–3
stored Fast/Balanced/Quality tiers; parsing keeps each preset's Fast (`-low`)
tier and drops the rest, but keeps all editing tiers, with a permanent one as
the default. Version-1 parsing maps known preset IDs onto the
starter presets and preserves prefixes; missing parameters are filled from
models. Read/migration never writes; explicit Save replaces the file with the
edited presets, omitting retired entries.
Validate unique preset IDs, nonblank bilingual names and finite numeric bounds (Steps 1–200,
CFG 0–100); protect assignments and capabilities, including attachment installs.
Tests cover bilingual labels, legacy reading, persistence, snapshots, preset
addition/removal and draft Save/Cancel with disposable files and local sockets.

Protocol v3 advertises `model-routing`; `model-routing.get/configure` return
completion messages with `routing`, and saves broadcast `model-routing.changed`.
Generation is routed by presetId alone; `qualityId` is rejected. Editing is
routed by an optional `qualityId` resolving that quality's model and Steps/CFG;
its prefixes lead the prompts sent to sd-cli and never pass through an LLM.
Resolve models server-side, reject conflicting modelId and legacy styleId mixing,
validate original prompts, then snapshot prefixes and preset defaults before
upload/queueing. Fill omitted Steps/CFG from the preset's snapshot; explicit valid
job values take precedence. Direct-model requests retain their existing defaults.
Editing adds only its quality's prefixes. Labels include `name` (English) and `nameFr`;
SDK displayLabel uses French for French locales, English otherwise.
Manager uses English names explicitly, even with a French system locale.
The Protocol target propagates MSVC's /utf-8 source encoding so bilingual labels
and consumer tests compile consistently on Windows.
Preserve legacy per-model style APIs/metadata without their management UI.
Deploy service first.

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

Image forms select installed VAE companions and eligible LLM text encoders by
opaque IDs in recommendedOptions.vaeModelId/textEncoderModelId. Save canEdit and
canGenerate there with at least one true; missing flags default true. LLM saves
canBeTextEncoder (default false) and the exclusive defaultPromptEnhancer and
defaultPromptTranslator roles (default false; saving a role clears it on every
other LLM in one metadata write). The service validates references on installation,
configuration and submission, protects referenced models during pending installs,
and prevents deletion/reclassification until detached. Execution uses --vae/--llm
and the selected LLM's assigned MMPROJ as --llm_vision. CLIP and image-level mmproj
attachments are retained on disk but not passed to sd-cli. Legacy VAE/LLM
attachments remain fallbacks. Clip Skip is removed from model configuration;
legacy request fields remain protocol-compatible. Editing jobs use image.edit.

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


## Identity portrait packages

The SDK IdentityPackage helper packages one to four ordered PNG references, face
and hair prompts, generation/edit usage prompts and seed metadata locally as a version-2 .kiraid ZIP. It contains
manifest.json (type kira-identity, version 2) and reference PNGs only.
Canvas exports it with a native Save dialog and atomic QSaveFile.
LibArchive remains the archive dependency; no identity extraction environment
or service-side extraction subprocess is required.

## Companion model assignments

The Companion category manages named VAE/mmproj entries in models/companion/.
The subtype is stored in recommendedOptions.subtype. MMPROJ requires GGUF.
LLM forms use a None/installed-MMPROJ selector instead of a companion URL.
Install/configure transports mmprojModelId and saves it atomically beside the LLM
metadata, preserving it when omitted and clearing it with an empty string.
Deletion/retyping of an assigned companion is rejected; deleting an LLM never
deletes a shared companion. Tests cover required names, subtype filtering,
assignment persistence, shared-file preservation and empty category panels.
Upscalers have no recommended options or explicit-content classification.
