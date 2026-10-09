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
  FIFO on the loopback llama-server (text.chat's `LlamaServerRunner`, with
  text.chat's default load settings so a chat client using them shares the
  loaded model; another model or setting restarts it), falling back to a
  llama-completion subprocess without llama-server, using saved temperature/topP/contextLength/
  maxTokens recommendations or shared LLM defaults. Per-job overrides accept these
  fields, a string systemPrompt up to 64 KiB and a boolean penalizeRepeats (default
  true; false disables the prompt-wide repetition penalty for faithful rewrites),
  without changing model metadata. Pass no top-k, min-p or sampler order, so the
  GGUF's general.sampling recommendations or llama.cpp's defaults apply.
  Unknown/invalid fields fail before queueing. Empty text requests a new description.
  Send the systemPrompt instructions at the start of the
  single user message, never through `--system-prompt-file`: llama-completion
  formats a system prompt alone, and templates requiring a user query (Qwen3.5
  and similar) abort on it.
  Preserve timeout/output limits, error recovery and disconnect cancellation.

- text.chat is a generic multi-turn chat: clients send system/user/assistant
  messages and receive one complete reply (never streamed). Keep it free of any
  client-domain logic. Chat-only overrides (stop, topK, minP, repeatPenalty,
  seed, engine, gpuLayers, kvCacheLocation, kvCacheType, flashAttention,
  jsonSchema, reasoning, DRY/XTC values, chatTemplate) never change model
  metadata, prompt.enhance or image jobs. A jsonSchema is client-owned: pass it
  to llama.cpp, verify the reply parses and return it decoded, but never
  interpret its fields. Accept only chatTemplate names listed by the engine's
  `--help`; llama.cpp treats any other string as a literal Jinja template. The default `server`
  engine is a loopback llama-server owned by `LlamaServerRunner` (random port,
  private API-key file, PDEATHSIG); a changed model or load setting restarts it.
  Unload it before every other job (prompt.enhance on it excepted) and after the
  idle period, so it never competes with sd-cli or llama-completion for GPU memory. `gpuLayers` other
  than `auto` passes `--fit off`: a model that does not fit must fail, not spill
  onto the CPU. text.chat.load and text.chat.unload are FIFO jobs: load takes
  text.chat's model and parameters without messages and keeps the first-chat
  lazy load; unload relies on the rule that every job not using llama-server
  stops it. Tests use the fake server and must never start a real one.

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
  capability flags, shared VAE/text encoder IDs and an optional promptFormat
  (natural, phrases or tags; default and fallback phrases, set manually in Manager,
  never inferred and never applied to execution; SDK ModelInfo::promptFormat());
  LLM uses temperature/top-p/context/max tokens
  and canBeTextEncoder (default false). LLM "Default prompt enhancer" and "Default prompt
  translator" checkboxes save exclusive defaultPromptEnhancer/defaultPromptTranslator
  booleans (default false): at most one LLM per role, the same LLM may hold both, and
  saving a role clears it on other LLMs atomically. Configure broadcasts `models.changed`
  to other clients; the SDK exposes defaultPromptEnhancerModelId()/defaultPromptTranslatorModelId().
  No job consumes the roles yet. Clip Skip has no configuration control. Upscalers have no
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

Sampling progress reports the step being sampled: sd-cli prints completed steps, so
currentStep is completed + 1 (capped at totalSteps; "sampling using" starts at 1) while
the percentage follows completed steps. sd-cli redraws its bar as "\r ... n/N - ..."
without a line end until the last step, so the runner also reads a complete "n/N -"
counter from unterminated output instead of waiting for the next redraw; repeated
counters are not re-emitted and the running step never moves backwards.

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


## Service-owned presets

Manager Presets… uses a shared KiraUi tree with fixed Generation and Editing roots.
Generation starts with Photorealism / Photoréalisme and Illustration. Add preset
and Remove preset icon buttons under the tree add a preset (generated unique
`preset-…` ID, default bilingual names, unassigned, one identity portrait, empty
prefixes) or remove the selected preset;
Generation keeps 1–16 presets. Preset IDs are never edited. Presets have no
quality layer: each carries exactly one ModelAssignment (modelId plus optional
Steps/CFG snapshot). Editing has the fixed qualities Fast / Rapide (`edit-low`),
Balanced / Équilibré (`edit-medium`) and Quality / Qualité (`edit-high`), each
with its own ModelAssignment; exactly one is the default (Balanced initially),
which clients preselect and use when they edit without a quality choice.
Qualities cannot be added, removed or renamed. Drag/drop and reparenting are
not permitted. Preset selection edits English/French names, Model, Steps, CFG
and optional positive/negative prefixes; editing quality selection edits Model,
Steps, CFG, optional positive/negative prefixes and a "Default quality" checkbox (checking it moves the default; the
default cannot be unchecked). Choosing a model snapshots its
recommendations, or shared defaults when absent. Save persists those values
including manual edits; catalogue refresh must not replace them. Unassigned
clears the assignment and snapshot.
Keep only Save/Cancel buttons; await acknowledgment and discard drafts on Cancel.

Store version-6 model-routing.json beside model metadata through QSaveFile:
presets carry modelId/steps/cfgScale and the top-level `editingQualities` array
carries each quality's id/name/nameFr/modelId/steps/cfgScale, boolean `default`
and positivePrefix/negativePrefix strings (optional on read, empty when absent;
at most 65536 characters).
Versions 4 and 5 stored one `editing` assignment, copied into every quality.
Each preset carries `identityViews` (1, 2 or 4; default 1): the number of
sequential identity portraits clients request with it. The service does not use
it for generation. Manager edits it as "Identity portraits" on the preset page
(hidden for editing qualities). Versions 1–4 load with 1.
Preset prompt flows are removed: presets hold no LLM steps, reads ignore a
`promptFlow` stored by earlier documents (versions 3–6) and Save no longer writes
it. LLM roles live only in the exclusive defaultPromptEnhancer/
defaultPromptTranslator LLM options; clients run them through prompt.enhance.
Transmit name (English) and nameFr for every preset and quality. SDK displayLabel
uses French for French locales, English otherwise. Validate 1–16 presets
with unique nonblank IDs (at most 64 characters) and nonblank English/French
names (at most 256 characters), the fixed editing qualities with exactly one
default, plus numeric bounds (Steps 1–200, finite CFG 0–100).
Versions 1–3 stored Fast/Balanced/Quality tiers: reads keep only each preset's
Fast (`photo-low`, `illustration-low`) tier as its assignment and drop the other
preset tiers, but keep all three editing tiers; a permanent editing tier becomes
the default (Balanced otherwise). Version-1 reads project
known preset IDs onto the fixed structure, retaining known prefixes. Missing
parameters are initialized from model recommendations.
Do not write during reads; Save writes version 6 and retires legacy entries.
Reject malformed/unreadable storage without overwriting. Block mapped-model
deletion and disabling required capabilities, including companion installs.
External disappearance leaves assignments intact.
Manager displays preset names in English explicitly.

Additive protocol-v3 model-routing.get/configure return .completed with routing;
broadcast model-routing.changed on Save and advertise model-routing capability.
Generation routing requires only presetId and rejects `qualityId`. image.edit is
routed by an optional `qualityId`: the service resolves the quality's model,
rejects a conflicting modelId, presetId or styleId, and fills omitted Steps/CFG
from its snapshot. After validating the original instruction, it composes the
quality's positive/negative prefixes first (comma-separated) onto the prompt and
negative prompt sent to sd-cli; prefixes never pass through an LLM
and are never returned to clients; an unknown or unassigned quality fails with
model-routing-unavailable. Edits without qualityId keep direct-model behavior.
Resolve the model on the service and reject conflicting modelId or mixed styleId.
Validate the original prompt, compose generation prefixes once, and snapshot before
upload/queueing. Omitted job Steps/CFG use the preset's snapshot; explicit valid
job values take precedence. Direct-model requests retain their prior behavior.
A routed generation never runs an LLM; its result returns the submitted
(trimmed) prompts without preset prefixes. There is no `prompt-flow.run`
operation or `skipPromptFlow` option; an unknown option is rejected.
Editing adds only its quality's prefixes, never preset prefixes. Preserve legacy style APIs/metadata for old clients,
without Manager UI. Canvas consumes this configuration; deploy service first.

## Identity package contract

The SDK IdentityPackage helper stores a compressed version-2 .kiraid ZIP:
manifest.json (type kira-identity, version 2), reference.png and optional ordered
reference-2.png through reference-4.png. Store face/hair descriptions, seed
metadata and generationPrompt/editPrompt instructions for using the references. Canvas generates portraits normally, then packages them locally through
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
