# Kira Service WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The version-3 authentication message is required for every client. Loopback peers may send an empty token. Non-loopback peers must send the configured shared LAN token.

The service replies with `authenticated` and `capabilities`. `models.list` returns
`image-generation`, `upscaler`, `llm-chat` and `companion`.
Each model contains `id`, `name`, `fileName` and `bytes`; a model with a
companion also includes the category-relative `companionFileName` and
`companionRole` (`mmproj` or `vae`).
## Prompt enhancement

prompt.enhance accepts an installed LLM (llm-chat) model ID, a prompt string
(up to 64 KiB, empty allowed), and optional parameters. Execution uses a
llama-completion subprocess in the shared FIFO. Recommendations default to the
standard LLM values: temperature 0.8, topP 0.95, contextLength 4096, maxTokens 1024.

```json
{"type":"submit","operation":"prompt.enhance","jobId":"enhance-1","options":{"modelId":"llm-chat:model.gguf","prompt":"a person","parameters":{"maxTokens":256,"systemPrompt":"Return JSON face and hair descriptions."}}}
```

Per-job overrides accept temperature, topP, contextLength and maxTokens with
standard LLM validation, and string systemPrompt up to 64 KiB. Unknown keys fail.
Overrides never change model recommendations. Empty input requests a new
description using the system instructions. Temperature zero selects greedy output.
The subprocess uses fixed repetition penalty 1.1 and top-k 20.
KIRA_LLAMA_COMPLETION selects the executable. Preserve the ten-minute timeout,
1 MiB output limit, single-job queue, disconnect cancellation and explicit errors.

Success returns one JSON message without binary data:

```json
{"type":"result","operation":"prompt.enhance","jobId":"enhance-1","prompt":"..."}
```

## Per-model prompt styles (additive v3 extension)

Image-generation catalogue entries include an ordered `styles` array:

```json
{"styles":[{"id":"cinematic","name":"Cinematic","positivePrefix":"cinematic lighting, dramatic composition, film still","negativePrefix":""}]}
```

Shared `PromptStyle` fields are required strings. IDs and names must be nonblank;
IDs must be unique within a model and at least one prefix must be nonblank.
IDs remain stable on rename; Manager assigns UUIDs to new custom entries.
Models without a saved styles field expose Photographic, Cinematic and Illustration
starters. Reads do not write metadata; explicitly saved `[]` stays empty.

Administrators replace the complete ordered list with:

```json
{"type":"models.styles.configure","id":"image-generation:model.gguf","styles":[{"id":"custom-id","name":"Custom","positivePrefix":"soft light","negativePrefix":"blur"}]}
```

Success returns `models.styles.configure.completed` with `id` and saved `styles`,
then broadcasts `models.styles.changed` with `id` to authenticated clients.
SDK/Manager refresh the catalogue on that notification. Invalid lists, missing
models, other categories and storage failures return `error` with model `id`
and code `invalid-model-styles`, without partial updates. Other metadata is retained.
SDK/Manager expose `configureModelStyles` and `modelStylesConfigured`.

`image.generate` options accept optional string `styleId`. Absent/empty means None and preserves existing prompt behavior.
Unknown/deleted IDs return `style-unavailable`. A nonempty selection on editing
or upscaling, or a non-string value, returns `invalid-request`; SDK submitEdit
rejects nonempty selections locally. The original user prompt must still be nonblank.
The service trims and joins each nonempty prefix and user prompt with `, `,
checks existing conditioning restrictions against composed prompts and snapshots
them before uploads/queueing. Later style changes cannot alter accepted jobs.
Clients send original prompts and never apply prefixes. Older services omitting
styles yield None only; deploy the service before using updated clients.

## Recommended model options (additive v3 extension)

Models may contain a `recommendedOptions` object in `models.list.result`,
`models.install.completed`, and `models.configure.completed`. Missing options
mean no recommendations have been saved. These are client recommendations;
job requests continue to determine execution settings. No paths are included.

`models.install` accepts the optional object. Omission preserves older clients;
when provided, the object must contain exactly the category's complete fields:

| Category | Fields and validation |
| --- | --- |
| image-generation | `width` and `height`: identical integers 256–2048, multiples of 256; `steps`: integer 1–200; `cfgScale`: number 0–100; `sampler` and `scheduler`: allowed strings; `clipSkip`: -1 or integer 1–12 |
| llm-chat | `temperature`: number 0–2; `topP`: number >0 and ≤1; `contextLength`: integer 256–131072; `maxTokens`: integer 1–contextLength |
| upscaler | No options: empty object only. No explicit-content classification. |
| companion | `subtype`: exactly `vae` or `mmproj`; MMPROJ requires GGUF. |

Samplers: empty (Automatic), `euler`, `euler_a`, `heun`, `dpm2`, `dpm++2m`, `lcm`.
Schedulers: empty (Automatic), `discrete`, `karras`, `exponential`, `ays`,
`sgm_uniform`, `simple`. Clip Skip -1 means Automatic. The GUI proposes defaults
from the shared Protocol API; the service does not infer model architecture.
Unknown fields, missing fields, wrong types and out-of-range values are rejected.

To replace recommendations on an installed model:

```json
{"type":"models.configure","id":"upscaler:model.gguf","displayName":"Upscaler","recommendedOptions":{}}
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

Image-generation models expose `modelType` (`checkpoint` by default,
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

Image-generation and LLM objects in model lists and install/configure completion
messages expose explicitContent, default false. It is separate from recommendations.
Upscaler and Companion objects omit this field.

For Image generation and LLM, `models.install` and `models.configure` accept optional `explicitContent`.
Upscaler and Companion omit classification; true is rejected for these categories.
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

Image-generation models accept optional `companionUrls` roles `llm`
(text encoder) and `mmproj` (vision projector). Manager add/optional-file dialogs
show both URLs for these categories; CLI add/attach accepts `--text-encoder` and
`--mmproj` together with the existing VAE/CLIP options. LLM models use --mmproj-model with an installed MMPROJ companion ID instead of a URL. mmproj must be GGUF; text encoders accept GGUF, safetensors,
or ckpt. Files are attached to their primary model, staged and published atomically,
preserved across restart, and deleted with the primary. Generation resolves them
internally and passes `--llm` and `--llm_vision` only when present. Generation catalog
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

`capabilities.operations` includes `image.edit` when the image backend is ready. Submit an installed Image-generation `modelId`, nonblank `prompt`, source `width`/`height`, and generation options through `submit` with operation `image.edit`. The service requires source bytes through the `upload-required` binary upload flow, even if `hasInput` is omitted. Optional `inputCount` (1–8, edit only, default 1) sets the number of images: the service sends one `upload-required` per image (`inputIndex` 0…n-1; role `source` for index 0, `reference` otherwise) and the client answers each with one binary frame in order. The first image is the one to edit; all are passed to sd-cli as repeated `-r` arguments in that order. Validation, queue/progress, result bytes and job errors follow the generation lifecycle. Results identify `image.edit`. Optional installed LLM, VAE and mmproj companions are resolved by the daemon; clients never supply paths. SDK clients use `submitEdit(ImageGenerationRequest)` and `ServiceCapabilities::imageEdit`.

### Preset prompt flows

A routed `image.generate` whose preset has a non-empty `promptFlow` runs its LLM
steps in order inside the same job, before image generation. Every step must
have an installed GGUF `llm-chat` model and `llama-completion` must be available;
otherwise submission fails with `model-routing-unavailable` (naming the step),
`llm-format-unsupported` or `backend-not-ready`. Each step receives the JSON
object `{"positive": string, "negative": string}` (the trimmed prompts) in a
single user message, after its instructions plus a fixed JSON reply contract
(no separate system prompt, which some chat templates reject without a user
query), and must reply with an object of the same shape; surrounding text
or code fences are ignored, and an omitted `negative` keeps the previous value.
Preset prefixes are composed once onto the final prompts after the last step.
Before each step the job emits `progress` with stage `enhancing-prompt`,
`percentage` 0, `statusText` (step name), 1-based `flowStep`, `flowSteps` and
`flowStepName`/`flowStepNameFr`. A failing step, timeout or unparsable reply
ends the job with `prompt-flow-failed`, naming the step.

The routed generation `result` message carries `prompt` and `negativePrompt`:
the final prompts without preset prefixes. Optional boolean `skipPromptFlow`
(routed generation only) submits the prompts as final prompts and runs no step.
SDK clients set `ImageGenerationRequest::skipPromptFlow` and read
`ImageJobResult::prompt`/`negativePrompt` and the `GenerationProgress` flow
fields.

`capabilities.operations` includes `prompt-flow.run` with `prompt.enhance`.
Submit operation `prompt-flow.run` with exactly `presetId`, `prompt` and optional
`negativePrompt` (strings of at most 64 KiB) to preview a preset's flow without
generating an image. It shares the single job queue, step validation, progress
and `prompt-flow-failed` errors; a preset without steps fails with
`prompt-flow-empty`. Success sends `{type: "result", operation:
"prompt-flow.run", prompt, negativePrompt}` with the final unprefixed prompts.
SDK clients use `canRunPromptFlow(presetId)`, `runPromptFlow(PromptFlowRequest)`,
`promptFlowFinished(PromptFlowResult)` and `ServiceCapabilities::promptFlow`.



## Local identity portrait packages

IdentityPackage is a local SDK archive helper, not a WebSocket operation. It stores
manifest.json (type kira-identity, version 2), one to four ordered PNG references,
face/hair prompts and seed metadata. No binary conditioning or extraction upload
is supported. Legacy package files remain untouched on disk.

## Image-generation models for editing

image.edit accepts every installed image-generation model ID through the same
source-image upload, companion resolution and generation-option validation as
before. No separate editing catalogue or eligibility flag is used. The removed
category's existing files remain on disk. Rebuild consumers against the updated SDK.

## Required display names and shared companions

New installs require displayName: a trimmed nonblank string of 1–256 characters.
Explicit empty renames fail; omitted names preserve the existing name on configure
or companion attachment. Names do not change opaque IDs or primary filenames.
Upscaler and Companion entries do not expose or accept explicit-content classification.

Standalone Companion files live in models/companion/, with VAE or MMPROJ subtype
stored under recommendedOptions.subtype. VAE accepts safetensors/ckpt/GGUF;
MMPROJ accepts GGUF. LLM install/configure accepts optional string mmprojModelId.
A nonempty ID must resolve to an installed MMPROJ entry; empty clears the assignment,
omission preserves it. Lists and completion messages return this ID. It never copies
the companion file. Deleting an LLM retains its shared companion; assigned companions
cannot be deleted or changed to VAE until every LLM is detached.
Legacy LLM companionUrl is rejected; use an installed companion ID.

```json
{"type":"models.install","category":"companion","installId":"vision","url":"https://example.com/projector.gguf","displayName":"Vision projector","recommendedOptions":{"subtype":"mmproj"}}
{"type":"models.install","category":"llm-chat","installId":"chat","url":"https://example.com/chat.gguf","displayName":"Chat","mmprojModelId":"companion:projector.gguf"}
```

Manager category/subtype selections have no configuration panel. Model add/save
requires a name. Upscalers have no recommended options, classification checkbox
or recommendations hint; LLM forms omit that hint and offer an installed-MMPROJ
selector. The current prompt-enhancement execution remains text-only.
