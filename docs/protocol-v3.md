# Kira Service WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The version-3 authentication message is required for every client. Loopback peers may send an empty token. Non-loopback peers must send the configured shared LAN token.

The service replies with `authenticated` and `capabilities`. `models.list` returns
`image-generation`, `edition`, `upscaler`, `llm-chat`, `ip_adapter`,
`clip_vision` and `photomaker`. `ip-adapter` remains an accepted category alias;
existing opaque `ip-adapter:` IDs and the storage directory are preserved.
Each model contains `id`, `name`, `fileName` and `bytes`; a model with a
companion also includes the category-relative `companionFileName` and
`companionRole` (`lora`, `mmproj` or `vae`). PhotoMaker does not accept companions.

Model administrators send `models.install` with a client-generated `installId`, explicit category, and HTTPS model-file URL. Optional `companionUrl` is an auxiliary LoRA for `ip_adapter`, mmproj for `llm-chat`, or VAE for `image-generation` and `edition`; it is rejected for `upscaler` and `clip_vision`. mmproj requires a `.gguf` filename; VAE and LoRA accept `.safetensors`, `.ckpt`, or `.gguf`. SHA-256 is no longer checked or accepted by the SDK/manager; legacy protocol `sha256` fields are ignored. The service stages both downloads and commits the primary, companion, and relationship metadata together, so a failed or canceled pair leaves no usable catalog entry. A restart preserves the relationship; deletion removes both files. Listings and install completion expose the companion filename and role, never absolute paths. Image-generation VAE companions are resolved internally and passed to sd-cli with --vae; image LLM text encoders and mmproj use --llm and --llm_vision respectively; LLM-chat mmproj does not alter image invocation. IP-Adapter auxiliary LoRA is consumed when the adapter is explicitly selected (see below). The service emits `models.install.accepted`, `models.install.progress`, `models.install.completed`, or an `error` containing that `installId`.

For example:

```json
{"type":"models.install","installId":"vision-1","category":"clip_vision","url":"https://example.com/clip_vision_h.safetensors","recommendedOptions":{"image_size":224,"normalize_input":true}}
{"type":"models.install","installId":"adapter-1","category":"ip_adapter","url":"https://huggingface.co/h94/IP-Adapter/resolve/main/models/ip-adapter_sd15.safetensors","recommendedOptions":{"strength":1.0}}
```

Model administrators may delete an installed model by its opaque ID:

```json
{"type":"models.delete","id":"upscaler:model.gguf"}
```

The service replies with `models.delete.completed` or an `error` containing the model `id`. Deletion is rejected while the model is referenced by active, queued, or pending-upload work. Filesystem paths are never accepted or returned.

Image work uses `submit` with `image.generate`, `image.upscale`, or `image.edit`; each request must include a matching installed `modelId`. Upscale is fixed at 2×. Input and result image data are binary WebSocket messages; filesystem paths are never part of the protocol.

## Recommended model options (additive v3 extension)

Models may contain a `recommendedOptions` object in `models.list.result`,
`models.install.completed`, and `models.configure.completed`. Missing options
mean no recommendations have been saved. These are client recommendations;
job requests continue to determine execution settings. No paths are included.

`models.install` accepts the optional object. Omission preserves older clients;
when provided, the object must contain exactly the category's complete fields:

| Category | Fields and validation |
| --- | --- |
| image-generation / edition | `width` and `height`: identical integers 256–2048, multiples of 256; `steps`: integer 1–200; `cfgScale`: number 0–100; `sampler` and `scheduler`: allowed strings; `clipSkip`: -1 or integer 1–12 |
| llm-chat | `temperature`: number 0–2; `topP`: number >0 and ≤1; `contextLength`: integer 256–131072; `maxTokens`: integer 1–contextLength |
| upscaler | `tileSize`: integer 64, 128, 256, 512 or 1024 |
| ip_adapter (legacy ip-adapter alias) | `strength`: number 0–2, default 1.0 |
| clip_vision | `image_size`: integer 1–4096, default 224; `normalize_input`: boolean, default true |
| photomaker | `style_strength`: integer 0–100, default 20 |

IP-Adapter options contain only `strength`. Clients such as Canvas explicitly
select the adapter and CLIP Vision models. Recommendations do not implicitly
enable an adapter or change job strength. CLIP Vision preprocessing remains owned by sd-cli: `image_size` and
`normalize_input` are saved/exposed recommendations, not unsupported CLI flags.

Samplers: empty (Automatic), `euler`, `euler_a`, `heun`, `dpm2`, `dpm++2m`, `lcm`.
Schedulers: empty (Automatic), `discrete`, `karras`, `exponential`, `ays`,
`sgm_uniform`, `simple`. Clip Skip -1 means Automatic. The GUI proposes defaults
from the shared Protocol API; the service does not infer model architecture.
Unknown fields, missing fields, wrong types and out-of-range values are rejected.

To replace recommendations on an installed model:

```json
{"type":"models.configure","id":"upscaler:model.gguf","recommendedOptions":{"tileSize":256}}
```

Success returns `models.configure.completed` with `id` and the complete `model`
object. Errors include the model `id` and `invalid-model-configure`. The SDK and
manager expose `configureModel()` and a `modelConfigured` signal. Only successful
service writes count as saved. Recommendations survive service restarts and are
removed with the model; existing companion relationships remain intact.

Only one model installation is active globally, including its sequential
companion download. Further requests fail with `model-install-busy` and the
request's `installId`; they are not queued. Image job queuing is unaffected.
Installation progress also includes `fileRole` (`primary` or `companion`), with
bytes and optional total for that file. The legacy SDK/manager Queued state
remains available for compatibility but the service no longer emits it.

```json
{"type":"models.install.cancel","installId":"client-generated-id"}
```

Only the installation's owning connection may cancel it. Successful cancellation
cleans temporary files and returns `models.install.canceled` with `installId`;
clients enter Canceled. Unknown or foreign IDs return `invalid-model-cancel`.
Disconnecting the owner also cancels its installation. Completion is reported
only after primary/companion files and metadata are committed; an installation
metadata failure rolls back its newly installed files.

On disk, category-local `metadata.json` retains legacy filename-to-companion
strings. Modified entries become objects containing optional
`companionFileName` and `recommendedOptions`. Untouched entries are preserved;
QSaveFile writes are atomic. Invalid/unreadable JSON must never be silently
replaced during install, configure, or delete.

## Image-generation option validation

`submit` / `image.generate` validates options before accepting a job or requesting
an input upload. Invalid types or ranges return `invalid-request` correlated to
`jobId`. Model recommendations are never applied implicitly.

| Field | Accepted values |
|---|---|
| prompt | Required non-empty string (after trimming) |
| negativePrompt | Optional string; defaults to empty |
| width, height | Optional integers 64–4096; each defaults to 1024; rectangular sizes are allowed |
| steps | Optional integer 1–200; defaults to 28 |
| cfgScale | Optional finite number 0–100; defaults to 7 |
| sampler, scheduler | Optional strings from the recommended-options lists; absent or empty means Automatic |
| clipSkip | Optional integer -1 or 1–12; defaults to -1 (Automatic) |
| seed | Optional integer -1–2147483647; defaults to -1 |
| hasInput | Optional boolean; defaults to false |
| baseImageStrength | Optional finite number 0–1; defaults to 0.55 |

Automatic sampler/scheduler flags are omitted from the backend command. Positive
Clip Skip values are forwarded; -1 omits the flag. CFG and image strength retain
numeric precision. The selected model's installed VAE is used for direct and
uploaded-input generation; clients neither submit nor receive its filesystem path.
Without an installed VAE companion, the backend uses the checkpoint's embedded
VAE. A diffusion-only file still requires the appropriate separate model weights;
numeric defaults cannot replace missing VAE or text-encoder weights.

## Optional image model files (additive v3 extension)

Image-generation and Edition models expose `modelType` (`checkpoint` by default,
`diffusion` for standalone UNets) and `companionFiles`, mapping `clip-l`, `clip-g`, `vae`, `llm`
and `mmproj` to category-relative companion filenames. Absolute paths are never
accepted or returned. The legacy single-companion fields remain compatible.

`models.install` accepts optional `companionUrls` mapping these roles to HTTPS
URLs, plus `modelType`. A request with an existing model `id` attaches missing
roles without downloading its primary or changing recommendations. Omit
`recommendedOptions` to preserve them. An omitted/empty modelType preserves the
existing type when attaching; new installations default to checkpoint. Duplicate roles, unsupported
roles, invalid URLs, existing destination files, and models in use are rejected.
Downloads use the existing install progress, owned cancellation, and completion
messages; `fileRole` may now be `clip-l`, `clip-g`, `vae`, `llm` or `mmproj` as well as legacy
`primary`/`companion`. Failures remove newly staged/published files, preserving
pre-existing model files and metadata. A role's presence means a file is attached,
not that arbitrary components are mutually compatible.

```json
{"type":"models.install","installId":"attach-123","category":"image-generation","id":"image-generation:existing.gguf","modelType":"diffusion","companionUrls":{"clip-l":"https://example.com/clip_l.safetensors","clip-g":"https://example.com/clip_g.safetensors","vae":"https://example.com/vae.safetensors"}}
```

The daemon snapshots attached file paths for each image job. Only registered
files are passed to the backend; neither folder scanning nor filename/extension
heuristics determine model type. Unused companions remain optional.

## Explicit-content classification

Every model object in `models.list.result`, `models.install.completed` and
`models.configure.completed` includes `explicitContent`, a boolean that defaults
to `false` for models without saved classification. It is separate from
`recommendedOptions` and applies to all seven categories.

`models.install` and `models.configure` accept optional `explicitContent`.
Non-boolean values are rejected. Omission leaves existing classification unchanged
(including companion attachments); new models default to false. Configuration
saves classification and recommendations atomically in category-local
`metadata.json`, preserving companion fields and untouched legacy entries.

```json
{"type":"models.configure","id":"image-generation:model.gguf","recommendedOptions":{"width":1024,"height":1024,"steps":28,"cfgScale":7,"sampler":"","scheduler":"","clipSkip":-1},"explicitContent":true}
```

The SDK exposes `ModelInfo::explicitContent` and optional final classification
arguments on `installModel()` and `configureModel()`. The service returns the
complete catalogue; application filtering is local and does not change execution
or service permissions.

Image-generation and Edition models accept optional `companionUrls` roles `llm`
(text encoder) and `mmproj` (vision projector). Manager add/optional-file dialogs
show both URLs for these categories; CLI add/attach accepts `--text-encoder` and
`--mmproj` together with the existing VAE/CLIP options. LLM-chat `--mmproj` keeps
its legacy meaning. mmproj must be GGUF; text encoders accept GGUF, safetensors,
or ckpt. Files are attached to their primary model, staged and published atomically,
preserved across restart, and deleted with the primary. Generation resolves them
internally and passes `--llm` and `--llm_vision` only when present. Edition catalog
support does not introduce a new editing job operation.

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

### Reference-image editing

`capabilities.operations` includes `image.edit` when the image backend is ready. Submit an installed Edition `modelId`, nonblank `prompt`, source `width`/`height`, and generation options through `submit` with operation `image.edit`. The service requires source bytes through the existing `upload-required` binary upload flow, even if `hasInput` is omitted. Validation, queue/progress, result bytes and job errors follow the generation lifecycle. Results identify `image.edit`. Optional installed LLM, VAE and mmproj companions are resolved by the daemon; clients never supply paths. SDK clients use `submitEdit(ImageGenerationRequest)` and `ServiceCapabilities::imageEdit`.


### IP-Adapter conditioning

Protocol v3 adds `capabilities.ipAdapter` (boolean), true when the image backend
is available. The SDK exposes `ServiceCapabilities::ipAdapter`; an older server
that omits it cannot accept SDK IP-Adapter submissions. Existing requests without
adapter fields and their single binary upload flow remain unchanged. Updated
SDK/Manager clients recognize both category spellings; consumers that need
IP-Adapter discovery must recognize the canonical `ip_adapter` category.

`clip_vision` requires one CLIP Vision weight file. `ip_adapter` requires one
IP-Adapter weight file and accepts one optional auxiliary LoRA through
`companionUrl` or `companionUrls.lora`. Both primary categories accept `.bin`,
`.safetensors`, `.ckpt`, and `.gguf`, retaining existing IP-Adapter file validation;
filename acceptance is not a guarantee of backend tensor compatibility. Auxiliary
LoRA accepts `.safetensors`, `.ckpt`, or `.gguf`. Installation/import, cancellation,
atomic publication, configuration, and deletion use the existing model lifecycle.
The Manager's Optional files dialog and CLI `attach <id> --companion-lora <url>`
can attach or replace an auxiliary LoRA (remove the existing role first).
No CLIP Vision companion roles are accepted.

Generation and editing optionally accept the following `options` fields together:

| Field | Meaning |
| --- | --- |
| `clipVisionModelId` | Installed opaque CLIP Vision model ID |
| `ipAdapterModelId` | Installed opaque IP-Adapter model ID |
| `hasIpAdapterImage` | Must be boolean true; requests the reference upload |
| `ip_adapter_strength` | Optional finite number 0–2; omitted means 1.0 |

Incomplete combinations, wrong model categories, unavailable weights, malformed
strength, and any adapter fields on upscale are rejected before upload. Example:

```json
{"type":"submit","operation":"image.generate","jobId":"adapter-job","options":{"modelId":"image-generation:sd15.safetensors","prompt":"a landscape","clipVisionModelId":"clip_vision:clip_vision_h.safetensors","ipAdapterModelId":"ip-adapter:ip-adapter_sd15.safetensors","hasIpAdapterImage":true,"ip_adapter_strength":0.8}}
```

`upload-required` now includes `inputRole`. Send one binary image message for each
request, in the requested order:

- `source`: the existing edit/upscale/input image (absent for text-to-image).
- `ip-adapter`: the distinct IP-Adapter reference image.

Editing with conditioning requests the source first, then the adapter image.
Queueing begins only after all requested images arrive. Each image has the existing
size limit; empty/oversized uploads fail the job and release its pending state.
Only one submission may await uploads per connection; a concurrent submission
returns `upload-in-progress` without replacing the pending job. Disconnection
releases pending uploads. The selected primary, adapter and vision weights are
protected against deletion/companion mutation while uploading, queued or active.

The daemon writes uploads to private temporary job files and resolves model IDs
internally, then supplies `--clip_vision`, `--ip-adapter`, `--ip-adapter-image`, and
`--ip-adapter-strength`. Editing also retains its separate `--ref-image`; a
text-to-image adapter reference never becomes an `--init-img`. The optional
auxiliary LoRA uses its private directory with `--lora-model-dir` and a generated
`<lora:filename:1>` prompt tag. With an auxiliary LoRA attached, user-supplied
LoRA tags and filenames that cannot form an unambiguous tag are rejected.
Clients cannot supply model or image filesystem paths.

SDK installation calls retain the accepted `ip-adapter` wire alias for older
servers; listing names on the updated service are canonical.

SDK `ImageGenerationRequest` adds `clipVisionModelId`, `ipAdapterModelId`,
`ipAdapterImageData`, and optional `ipAdapterStrength`. Use them with
`submitGeneration` or `submitEdit`; `baseImageData` remains the independent source
image. The SDK answers each upload role with the corresponding bytes. Explicit
IDs, reference bytes and advertised support are required; recommendations never
select or activate an adapter automatically.

Use weights compatible with the installed sd-cli and base model. Upstream
[IP-Adapter documentation](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/ip_adapter.md)
describes classic/Plus adapters for SD 1.5 and SDXL with a compatible CLIP Vision
encoder. An Edition request can carry the same conditioning fields, but its base
model must support IP-Adapter in sd-cli; this does not add support to incompatible
architectures. PhotoMaker uses the separate operation below; InstantID, DINOv2
and IP-Adapter FaceID pipelines remain outside this implementation.

## PhotoMaker V2 identity export

`models.list` additionally returns `photomaker`. Its required primary filename
ends in `.safetensors`; companions are rejected. Install/configure use the usual
atomic model workflow and `recommendedOptions: {"style_strength":20}`.
`style_strength` must be an integer from 0 through 100. It recommends sd-cli's
`--pm-style-strength`; it is not an extraction parameter. PhotoMaker and
IP-Adapter catalogue IDs/options are independent.

When the service-host extractor is configured, `capabilities.operations`
includes `photomaker.export`, independently of sd-cli image capabilities.
The SDK exposes `ServiceCapabilities.photoMakerIdentityExport`.

```json
{"type":"submit","operation":"photomaker.export","jobId":"identity-job","options":{}}
{"type":"upload-required","jobId":"identity-job","inputRole":"source"}
```

The client sends one binary message containing the source image (full or cropped).
No model, prompts or generation options are accepted. The source must contain
exactly one detectable face. Uploads follow the existing image-size limit.
Normal queued/queue-position/running events use the same FIFO as image jobs.
The server creates temporary storage, saves the image, executes the official
PhotoMaker V2 extractor and returns:

```json
{"type":"result","operation":"photomaker.export","jobId":"identity-job","mimeType":"application/octet-stream","fileName":"id_embeds.bin"}
```

The next binary message contains only the unchanged identity-file bytes.
There is no base64/JSON encoding, generated image, service path or persistent
identity record. Temporary files are removed on success/failure and after
process cancellation on client disconnection.

Job-scoped error codes:

- `photomaker-unavailable`: extraction is not configured.
- `photomaker-no-face`: no face detected after the official fallback attempts.
- `photomaker-multiple-faces`: multiple detected faces make the source ambiguous;
  provide a crop containing one face.
- `photomaker-extraction-failed`: interpreter/dependency/model/inference failure,
  timeout, or unreadable/empty output.
- `photomaker-output-missing`: successful process did not produce id_embeds.bin.

The SDK call `exportPhotoMakerIdentity(QByteArray)` returns a job ID or an empty
ID if disconnected, unsupported or given empty input. Completion/failure emits
`photoMakerIdentityExportFinished(PhotoMakerIdentityResult)`, with `jobId`,
`succeeded`, `identityData`, `errorCode` and `message`. Connection failures
also complete this dedicated result; `imageJobFinished` is never emitted for
an identity export. Clients need not know the script, interpreter or server paths.

## PhotoMaker V2 generation

`capabilities.photoMaker` advertises generation conditioning when the image
backend is available, independently of extraction dependencies. Missing means
unsupported. Use `image.generate` with the usual prompts and generation values,
an installed SDXL `modelId`, an installed `photoMakerModelId`, boolean
`hasPhotoMakerIdentity: true`, and optional integer `photoMakerStyleStrength`
(0–100, default 20). Recommendations are not applied automatically.

```json
{"type":"submit","operation":"image.generate","jobId":"pm-job","options":{"modelId":"image-generation:sdxl.safetensors","prompt":"a woman img, in a garden","photoMakerModelId":"photomaker:photomaker-v2.safetensors","hasPhotoMakerIdentity":true,"photoMakerStyleStrength":20}}
```

The service requests two binary uploads in order: `inputRole` is first
`photomaker-reference` (one decodable reference image), then `photomaker-identity`
(the original opaque embedding bytes exported for that image). Both are required:
sd-cli uses visual features from the reference as well as the V2 embeddings.
The filename on the client is irrelevant. Each upload uses the configured
maximumImageBytes limit; empty or oversized uploads release the pending job.
The embeddings must describe exactly one reference image. The prompt must include
a class word followed by the `img` trigger, such as `woman img` or `man img`.
Model compatibility and embedding contents are checked by sd-cli, not the catalog.

The SDK exposes `ImageGenerationRequest.photoMakerModelId`,
`photoMakerIdentityPackageData` (complete `.kiraid` bytes) and optional
`photoMakerStyleStrength` through `submitGeneration`. Its LibArchive helper
validates and unpacks the package in memory and sends the two existing binary
upload roles above; no new wire operation or server filesystem path is introduced.
Version 1 is a compressed ZIP with exactly `reference.png`, `id_embeds.bin` and
`manifest.json` containing `{"type":"photomaker-v2","version":1}`. Only regular
entries are accepted; duplicate names, unexpected entries and excessive expanded
sizes are rejected. The crop and opaque embeddings have a combined 64 MiB limit.
The legacy `photoMakerReferenceImageData` / `photoMakerIdentityData` pair is still
accepted, but cannot be supplied together with a package. Missing capability, model
or bytes prevents submission. PhotoMaker cannot be combined with IP-Adapter,
image-to-image input, editing or upscaling in this slice.

The service protects both selected models during upload, queueing and execution,
saves private temporary input files and passes `--photo-maker`,
`--pm-id-embed-path`, `--pm-id-images-dir` and `--pm-style-strength` to sd-cli.
Clients cannot supply filesystem paths. Results use the normal image job lifecycle
and binary image delivery; success/failure/disconnection cleans temporary files.
If sd-cli reports that PhotoMaker conditioning is disabled, the job fails rather
than returning an unconditioned image. Extraction Python dependencies are not
needed to reuse existing embeddings.
See [upstream PhotoMaker documentation](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/photo_maker.md).
