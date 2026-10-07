# Kira Service Development Contract

## Reviewable and reversible edits

- Use the `apply_patch` tool for all manual text-file additions, edits, and deletions. Do not use Python, Perl, sed, shell redirection, or other scripts to rewrite project files.
- Project generators and formatters may update files when required by the task. Identify these tool-generated changes and review their diffs.
- Inspect Git status before editing, preserve pre-existing user changes, and keep edits uncommitted unless the user requests a commit. Review the final diff, run `git diff --check`, and report changed files, including new untracked files.
- Keep changes small so they can be reviewed and undone independently. When asked to undo work, revert only the agent's changes while preserving subsequent user edits; never use a broad reset or discard unrelated changes.

## Development rules

- Kira Service is a C++20 Qt 6 backend project. Keep the daemon and native manager separate executables, and keep reusable image transport code in the installable SDK under `src/sdk/`.
- The daemon owns model storage, a single active HTTPS model installation, recommended model options, temporary jobs, and a FIFO execution queue for `sd-cli` image jobs. It never exposes service paths over WebSocket.
- The manager supports GUI and `list`/`add`/`delete` CLI modes and is installed into the invoking user's `~/.local/bin`. CLI remains local at `127.0.0.1:9384`. GUI uses the last authenticated Service/Host and Service/Token from QSettings, defaulting to loopback with no token. On startup connection/authentication failure or timeout, show a shared connection dialog accepting IPv4 or localhost and a masked key, building ws:// on fixed port 9384. Persist only after authentication; failure retains the draft and Cancel preserves saved settings. Do not repeatedly prompt during background reconnects. LAN connections require a configured listener/token and a trusted network. Browse remains a local-only import workflow.
- Sources are organized under `src/daemon/`, `src/manager/`, and `src/sdk/`; the daemon and manager executable targets remain declared by `src/CMakeLists.txt`. Export `KiraService::Protocol` and `KiraService::SDK` from the SDK.
- The daemon always listens on `127.0.0.1`; a configured LAN IPv4 address adds a second listener on the same port. Loopback authentication is tokenless; LAN authentication remains token-protected.
- Preserve protocol v3 unless an explicit compatibility slice changes it. Canvas and other clients select opaque model IDs and submit work; Canvas may expose installation only for explicitly curated packages through the SDK, while the local manager remains the general model-management UI.
- CMake installs the daemon to `libexec/kira-service/` and the manager to `bin/` under the selected prefix; the shell installer remains authoritative for systemd deployment and configuration.
- CMake also installs SDK headers and static libraries and generates the `KiraServiceSdk` package. The SDK supports image operations.
- Use `$XDG_DATA_HOME/kira-service/models/` with `upscale/`, `llm/` and `companion/` subdirectories. Generation models in the root also serve image.edit with reference-image conditioning. Never overwrite installed model files. Keep the legacy-root migration safe and non-merging.
- Linux systemd deployment keeps the `kira-service` account, `/etc/kira-service`, `/var/lib/kira-service`, `/run/kira-service`, and fixed port `9384`.
- Shared VAE and LLM text encoders are assigned by opaque IDs; LLM MMPROJ uses a shared companion ID. Never copy/delete shared weights with a primary model. Keep legacy companionUrl, companionFileName, companionUrls/companionFiles and string metadata compatible without restoring removed CLIP/image-level mmproj controls or execution arguments. Legacy VAE/LLM attachments remain fallbacks. Existing attachment operations never redownload or overwrite the primary. Stage files and publish metadata atomically; cancellation/failure preserves an existing primary and recommendations. Do not perform SHA-256 download checks. Diffusion models use --diffusion-model and checkpoints use --model. Validate generation values/types before queuing, omit automatic sampler/scheduler flags, and preserve numeric precision. Never auto-apply numeric recommendedOptions to jobs.
- Build and run CTest after each slice. Keep README and CONTRIBUTING accurate.
- Missing image-generation dimensions/steps use GenerationRequest defaults (1024×1024, 28), while supplied invalid values remain errors. Keep absent CFG at 7 and automatic backend choices omitted. An external VAE remains optional for complete checkpoints; never substitute numeric defaults for missing model weights.

## Manager UI and recommended options

Generation models serve image.generate and/or image.edit according to saved
recommendedOptions.canGenerate/canEdit (missing flags default true). At least one
must be true. Both operations resolve original generation
IDs, model types and companions from the generation directory. Existing legacy
directories and metadata remain on disk, without migration or deletion.

A nonblank displayName (trimmed, 1–256 characters) is mandatory for every new
installation and save. Empty renames are rejected; omission preserves an existing
name. Manager add/save buttons require a name. CLI add requires --display-name.
Names are labels only; keep opaque IDs and never rename existing weight files.
Category/subtype selections leave the right configuration panel empty.
Primary filename collisions receive a numeric suffix before the extension;
never overwrite weights and keep companions scoped to the final primary name.

- Prompt enhancement executes installed LLM (llm-chat) models through the shared
  FIFO and llama-completion subprocess, using saved temperature/topP/contextLength/
  maxTokens recommendations or shared LLM defaults. Per-job overrides accept these
  fields and a string systemPrompt up to 64 KiB, without changing model metadata.
  Unknown/invalid fields fail before queueing. Empty text requests a new description.
  Send instructions (systemPrompt or flow step instructions) at the start of the
  single user message, never through `--system-prompt-file`: llama-completion
  formats a system prompt alone, and templates requiring a user query (Qwen3.5
  and similar) abort on it.
  Preserve timeout/output limits, error recovery and disconnect cancellation.

- Link only the manager/UI tests to KiraUi::Widgets 0.2. Default to the sibling
  checkout, allow KIRA_UI_SOURCE_DIR, and fall back to an installed package.
  Keep daemon and SDK independent of the visual framework.
- Use native OS title bars and window/dialog buttons; keep the Kira content theme.
- Apply KiraUi::Theme only in GUI mode. Use the shared MainWindow/Dialog, panels,
  tree and controls. Preserve the local CLI; GUI may connect to an authenticated LAN service.
- Use a landscape model tree/details splitter with a fixed, non-resizable window
  content size of 1100×640 logical pixels, excluding the native title bar. Restore
  saved position but ignore previous sizes and maximized states. Persist
  MainWindow/Geometry and MainWindow/ModelsSplitter with Qt.
- Reuse one options form for installed model editing and the add/download modal.
  Confirm unsaved edits; wait for service acknowledgment before reporting Save.
- Install one model (and optional companion) at a time globally. Refuse concurrent
  requests with model-install-busy; keep the image execution FIFO unchanged.
  Closing the download modal cancels its owned install; success closes only after
  all requested files and metadata are saved. Errors preserve entered fields for retry.
- Protocol v3 adds optional recommendedOptions, models.configure, and owned
  models.install.cancel. Keep old client calls and enum states source-compatible.
  Options are recommendations only; do not silently change execution arguments.
- ModelOptions in the Protocol target owns category defaults and validation.
  Image generation use square resolution, steps, CFG, sampler, scheduler,
  capability flags and shared VAE/text encoder IDs; LLM uses temperature/top-p/context/max tokens
  and canBeTextEncoder (default false). Clip Skip has no configuration control. Upscalers have no
  recommended options or explicit-content control. Companion models expose only
  their VAE/mmproj subtype; hide unrelated fields and recommendations hints.
- Preserve legacy metadata.json companion strings and untouched entries when
  adding object entries. Write atomically with QSaveFile; reject mutation on
  unreadable metadata. Keep existing models without recommendations usable.
- Embed the manager PNG icon exports at startup in GUI mode. Keep generated master
  artwork and provenance under assets/icons; do not add runtime asset conversion.
- Keep documentation in English. UI tests use isolated settings and disposable
  local service fixtures; never mutate live installed models to exercise tests.

- Image-generation and LLM models expose a top-level `explicitContent` boolean, default false.
  The Manager's shared add/edit form owns its checkbox. Install/configure accept
  an optional boolean; omission preserves existing classification. Store it atomically
  with recommendations in category-local metadata, preserving legacy and companion
  fields. Return it in lists and install/configure completion. Keep protocol v3;
  consumer catalogue filtering is local and does not alter execution permissions.

Image-generation forms select installed Companion/VAE and eligible LLM entries
through recommendedOptions.vaeModelId/textEncoderModelId, with None. These shared
references are validated and protected against deletion/reclassification, including
pending installs; image model deletion never deletes shared weights. Execution
uses --vae/--llm and the selected LLM's mmprojModelId as --llm_vision. CLIP-L,
CLIP-G and image-level mmproj controls/arguments are removed. Legacy attachment
APIs/CLI and files remain compatible; VAE/LLM files serve as fallbacks when no
shared assignment exists. Optional files retains legacy VAE removal/replacement.
LLM models select a separately installed MMPROJ companion by opaque ID; no LLM
mmproj URL is accepted. Editing jobs use image.edit with uploaded reference-image bytes.

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
Temporary testing override: the shared image generation runner passes
`--diffusion-fa` exactly once for generation and editing.
Per-model configuration is deferred; do not add Manager controls in this slice.

Linux remote updates use `scripts/update-kira-service.sh` as the checkout owner:
fetch and fast-forward the configured upstream, configure/build/test, then invoke
the existing systemd installer with `sudo -n`. Keep installation gated on passing
tests and retain existing installer configuration/model-preservation behavior.
Sibling checkouts and sd-cli are not updated by this script.


## Service-owned presets and qualities

Manager Presets… uses a shared KiraUi tree with fixed Generation and Editing roots.
Generation presets are Photorealism / Photoréalisme and Illustration. Each preset
and Editing have fixed Fast / Rapide, Balanced / Équilibré and
Quality / Qualité qualities, with stable IDs and order.
No addition, removal, rename, drag/drop or reparenting is permitted.
Preset selection edits optional positive/negative prefixes; quality selection
edits Model, Steps and CFG. Choosing a model snapshots its recommendations, or
shared defaults when absent. Save persists those values including manual edits;
catalogue refresh must not replace them. None clears the assignment and snapshot.
Keep only Save/Cancel buttons; await acknowledgment and discard drafts on Cancel.
Each quality exposes a Permanent quality checkbox, saved as an optional boolean
`permanent` (default false) in routing. Permit at most one checked
quality independently within each preset and Editing. Checking another clears
the previous choice only in that group. Canvas uses the permanent profile and
hides that group's slider; no checked quality restores normal selection.

Store version-3 model-routing.json beside model metadata through QSaveFile.
Each generation preset carries an ordered `promptFlow` of at most 4 LLM steps
(id, name, nameFr, modelId, instructions). Step IDs are unique within the preset,
bilingual names are nonblank, instructions are at most 65536 characters and
modelId is an installed `llm-chat` model or empty (unassigned). Assigned flow
LLMs cannot be deleted; reconfiguring them is allowed. Versions 1 and 2 load
with empty flows. In Manager, a "Prompt flow (n steps)" node follows each
preset's fixed qualities; its page lists steps with Add/Move up/Move down/Remove
icon buttons (Add disabled at 4) above the selected step's English/French names,
LLM selector (Unassigned, installed llm-chat models, Unavailable — id) and
instructions. New steps get a generated unique ID and default names; flow edits
follow the same Save/Cancel draft rules.
Transmit name (English) and nameFr for every preset/quality. SDK displayLabel
uses French for French locales, English otherwise. Validate fixed IDs, ordering
and bilingual labels plus numeric bounds (Steps 1–200, finite CFG 0–100).
Version-1 reads project known IDs onto the fixed structure, retaining known
prefixes/assignments and initializing missing parameters from model recommendations.
Do not write during reads; Save writes version 3 and retires legacy custom entries.
Reject malformed/unreadable storage without overwriting. Block mapped-model
deletion and disabling required capabilities, including companion installs.
External disappearance leaves assignments intact.
Manager displays preset/quality names in English explicitly. Keep existing
low/medium/high IDs when renaming labels to Fast/Balanced/Quality. Accept previous
version-2 labels on read and normalize only names; retain model assignments,
Steps/CFG, prefixes and selection IDs. Do not write until explicit Save.

Additive protocol-v3 model-routing.get/configure return .completed with routing;
broadcast model-routing.changed on Save and advertise model-routing capability.
Generation routing requires presetId/qualityId; editing requires only qualityId.
Resolve the model on the service and reject conflicting modelId or mixed styleId.
Validate the original prompt, compose generation prefixes once, and snapshot before
upload/queueing. Omitted job Steps/CFG use quality snapshots; explicit valid job
values take precedence. Direct-model requests retain their prior behavior.
A routed generation runs its preset's prompt flow first, as described in
docs/protocol-v3.md: each step's LLM receives/returns `{positive, negative}`
JSON through llama-completion, progress uses stage `enhancing-prompt`, failures
use `prompt-flow-failed`, and prefixes are composed after the last step instead
of at submission. Results return the final unprefixed prompts; `skipPromptFlow`
bypasses the flow. Validate every step's LLM at submission. The
`prompt-flow.run` operation previews a preset's flow through the same job queue
and returns only the final prompts.
Editing adds no prefixes. Preserve legacy style APIs/metadata for old clients,
without Manager UI. Canvas consumes this configuration; deploy service first.

## Identity package contract

The SDK IdentityPackage helper stores a compressed version-2 .kiraid ZIP:
manifest.json (type kira-identity, version 2), reference.png and optional ordered
reference-2.png through reference-4.png. Store face/hair descriptions and seed
metadata. Canvas generates portraits normally, then packages them locally through
LibArchive and QSaveFile. There is no binary extraction or conditioning payload.
Reject malformed, duplicate, oversized or unexpected archive entries.
Removed conditioning/extraction APIs and model categories must not be restored.
Existing user model directories and packages are retained on disk.

Companion is a dedicated model category with recommendedOptions.subtype equal to
vae or mmproj. VAE accepts safetensors/ckpt/GGUF; MMPROJ requires GGUF. Standalone
companions have required names and no explicit-content classification.
LLM install/configure accepts optional string mmprojModelId: omission preserves,
empty clears, and nonempty must reference an installed mmproj subtype. Save this
ID atomically in LLM metadata; list it through the SDK. Never copy or delete the
referenced weight as part of an LLM operation. Reject deleting or retyping an
assigned companion and protect references during pending installation.
The Manager add/edit LLM selector offers None and installed MMPROJ entries by ID,
preserving drafts during catalogue refreshes. Text prompt enhancement remains
text-only; this assignment does not introduce image chat.
