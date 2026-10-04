# Kira Service Development Contract

## Reviewable and reversible edits

- Use the `apply_patch` tool for all manual text-file additions, edits, and deletions. Do not use Python, Perl, sed, shell redirection, or other scripts to rewrite project files.
- Project generators and formatters may update files when required by the task. Identify these tool-generated changes and review their diffs.
- Inspect Git status before editing, preserve pre-existing user changes, and keep edits uncommitted unless the user requests a commit. Review the final diff, run `git diff --check`, and report changed files, including new untracked files.
- Keep changes small so they can be reviewed and undone independently. When asked to undo work, revert only the agent's changes while preserving subsequent user edits; never use a broad reset or discard unrelated changes.

## Development rules

- Kira Service is a C++20 Qt 6 backend project. Keep the daemon and native manager separate executables, and keep reusable image transport code in the installable SDK under `src/sdk/`.
- The daemon owns model storage, a single active HTTPS model installation, recommended model options, temporary jobs, and a FIFO execution queue for `sd-cli` image jobs. It never exposes service paths over WebSocket.
- The manager is local-only: it connects to `127.0.0.1:9384`, supports GUI and `list`/`add`/`delete` CLI modes, and is installed into the invoking user's `~/.local/bin`.
- Sources are organized under `src/daemon/`, `src/manager/`, and `src/sdk/`; the daemon and manager executable targets remain declared by `src/CMakeLists.txt`. Export `KiraService::Protocol` and `KiraService::SDK` from the SDK.
- The daemon always listens on `127.0.0.1`; a configured LAN IPv4 address adds a second listener on the same port. Loopback authentication is tokenless; LAN authentication remains token-protected.
- Preserve protocol v3 unless an explicit compatibility slice changes it. Canvas and other clients select opaque model IDs and submit work; Canvas may expose installation only for explicitly curated packages through the SDK, while the local manager remains the general model-management UI.
- CMake installs the daemon to `libexec/kira-service/` and the manager to `bin/` under the selected prefix; the shell installer remains authoritative for systemd deployment and configuration.
- CMake also installs SDK headers and static libraries and generates the `KiraServiceSdk` package. The SDK supports image operations.
- Use `$XDG_DATA_HOME/kira-service/models/` with `upscale/`, `llm/`, and `edition/` subdirectories. The `edition` category manages image-editing model files; image.edit jobs use reference-image conditioning. Never overwrite installed model files. Keep the legacy-root migration safe and non-merging.
- Linux systemd deployment keeps the `kira-service` account, `/etc/kira-service`, `/var/lib/kira-service`, `/run/kira-service`, and fixed port `9384`.
- Optional companions use category-local persistent metadata: mmproj for LLM-chat, VAE for Image-generation/Edition, and LoRA for IP-Adapter. Install and delete them with their primary model; expose only category-relative companion filenames and roles. Do not perform SHA-256 download checks. Image-generation/Edition also support role-specific optional CLIP-L, CLIP-G, VAE, LLM text encoder and mmproj files through companionUrls/companionFiles, with modelType checkpoint (default) or diffusion. Existing image models accept companion attachment via models.install with an opaque id; never redownload or overwrite the primary. Stage all files and publish metadata atomically; cancellation/failure must preserve an existing primary and recommendations. Keep legacy companionUrl, companionFileName and string metadata compatible. Image-generation companions are resolved internally and passed with --clip_l, --clip_g, --vae, --llm and --llm_vision; diffusion models use --diffusion-model and checkpoints use --model; LLM-chat mmproj remains a catalog attachment. An explicitly selected IP-Adapter uses its optional auxiliary LoRA through --lora-model-dir and a generated prompt tag. Validate generation values/types before queuing, omit automatic sampler/scheduler flags, and preserve numeric precision. Never auto-apply recommendedOptions to jobs.
- Build and run CTest after each slice. Keep README and CONTRIBUTING accurate.
- Missing image-generation dimensions/steps use GenerationRequest defaults (1024×1024, 28), while supplied invalid values remain errors. Keep absent CFG at 7 and automatic backend choices omitted. An external VAE remains optional for complete checkpoints; never substitute numeric defaults for missing model weights.

## Manager UI and recommended options

- Link only the manager/UI tests to KiraUi::Widgets 0.2. Default to the sibling
  checkout, allow KIRA_UI_SOURCE_DIR, and fall back to an installed package.
  Keep daemon and SDK independent of the visual framework.
- Use native OS title bars and window/dialog buttons; keep the Kira content theme.
- Apply KiraUi::Theme only in GUI mode. Use the shared MainWindow/Dialog, panels,
  tree and controls. Preserve the local CLI and fixed loopback connection.
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
  Image generation/Edition use square resolution, steps, CFG, sampler, scheduler,
  Clip Skip; LLM uses temperature/top-p/context/max tokens; Upscaler uses tile
  size; IP-Adapter uses strength; CLIP Vision uses image_size/normalize_input. Hide unrelated fields.
- Preserve legacy metadata.json companion strings and untouched entries when
  adding object entries. Write atomically with QSaveFile; reject mutation on
  unreadable metadata. Keep existing models without recommendations usable.
- Embed the manager PNG icon exports at startup in GUI mode. Keep generated master
  artwork and provenance under assets/icons; do not add runtime asset conversion.
- Keep documentation in English. UI tests use isolated settings and disposable
  local service fixtures; never mutate live installed models to exercise tests.

- All model categories expose a top-level `explicitContent` boolean, default false.
  The Manager's shared add/edit form owns its checkbox. Install/configure accept
  an optional boolean; omission preserves existing classification. Store it atomically
  with recommendations in category-local metadata, preserving legacy and companion
  fields. Return it in lists and install/configure completion. Keep protocol v3;
  consumer catalogue filtering is local and does not alter execution permissions.

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

Linux remote updates use `scripts/update-kira-service.sh` as the checkout owner:
fetch and fast-forward the configured upstream, configure/build/test, then invoke
the existing systemd installer with `sudo -n`. Keep installation gated on passing
tests and retain existing installer configuration/model-preservation behavior.
Sibling checkouts and sd-cli are not updated by this script.


IP-Adapter conditioning uses canonical categories `clip_vision` and `ip_adapter`;
keep legacy `ip-adapter` requests, opaque IDs/storage and strength-only options
compatible. CLIP Vision requires a primary weight and stores image_size (224) /
normalize_input (true); IP-Adapter requires primary weights, an optional auxiliary
LoRA, and strength (1.0). Never auto-apply these
recommendations. sd-cli owns preprocessing; do not invent CLI flags for these
metadata fields. Preserve model installation/configuration and atomic file behavior.

Generation/editing select both opaque IDs and upload a separate conditioning image.
Protocol v3 advertises capabilities.ipAdapter and upload-required.inputRole; source
then ip-adapter binary uploads queue one job only after both arrive. Protect selected
weights/companions while pending, queued or active; reject incomplete combinations
and adapter fields on upscale. SDK requests use clipVisionModelId, ipAdapterModelId,
ipAdapterImageData and optional ipAdapterStrength (wire ip_adapter_strength, 0–2,
default 1). Use --clip_vision/--ip-adapter/--ip-adapter-image/--ip-adapter-strength;
retain edit --ref-image separately. Optional auxiliary LoRA uses --lora-model-dir
and a generated <lora:filename:1> tag; reject user LoRA tags for these jobs.
PhotoMaker V2 is a separate `photomaker` category: one required .safetensors
primary, no companions, and integer style_strength (0–100, default 20).
Use existing model installation/configuration/storage conventions.
The additive protocol-v3 `photomaker.export` operation accepts one source image
(including a client-prepared crop) and returns only id_embeds.bin bytes through
the SDK's dedicated photoMakerIdentityExportFinished result. No image job result
or image generation occurs. Share the FIFO, keep temporary storage until process
termination, and clean it on success/failure/disconnection.
Run the official stable-diffusion.cpp scripts/face_detect.py through the bundled
strict single-face guard. Python/InsightFace are optional service-side extraction
dependencies only; Canvas never runs them. Configure KIRA_PHOTOMAKER_PYTHON and
KIRA_PHOTOMAKER_FACE_DETECT on the service host. Advertise extraction independently
of sd-cli when these paths are configured; dependency/model failures remain explicit.
Do not add InstantID, DINOv2, general identity abstractions, or a Canvas crop/export
UI in this slice.

PhotoMaker V2 generation is supported by image.generate and capabilities.photoMaker.
Resolve photoMakerModelId internally; require hasPhotoMakerIdentity and upload one
photomaker-reference image followed by photomaker-identity opaque embedding bytes.
The matching reference image remains required by sd-cli, even with existing embeddings.
SDK fields are photoMakerModelId, photoMakerReferenceImageData, photoMakerIdentityData
and optional photoMakerStyleStrength (integer 0–100, default 20). Do not combine
with editing, upscaling, image-to-image or IP-Adapter. Protect both models throughout
the lifecycle and clean temporary files. Use --photo-maker, --pm-id-embed-path,
--pm-id-images-dir and --pm-style-strength; report backend conditioning disablement
as failure. Prompts require a class word followed by img; callers select compatible
SDXL/PhotoMaker V2 weights. Reuse needs no Python extraction runtime.

## PhotoMaker identity package contract

The SDK's LibArchive-backed `.kiraid` helper owns the compressed ZIP format:
reference.png, id_embeds.bin, manifest.json (type photomaker-v2, version 1).
Generation accepts photoMakerIdentityPackageData and unpacks it in memory before
uploading existing protocol-v3 roles. Reject malformed, oversized and ambiguous
package-plus-paired inputs; preserve legacy paired requests. Extraction continues
to return only id_embeds.bin. No Python or archive executable runs in Canvas.
