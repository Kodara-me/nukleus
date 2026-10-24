# Nukleus WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The version-3 authentication message is required for every client. Loopback peers may send an empty token. Non-loopback peers must send the configured shared LAN token.

The service replies with `authenticated` and `capabilities`. `models.list` returns
`image-generation`, `upscaler`, `llm-chat`, `companion` and `animation`
(see [Animation models](#animation-models)).
Each model contains `id`, `name`, `fileName` and `bytes`; a model with a
companion also includes the category-relative `companionFileName` and
`companionRole` (`mmproj` or `vae`).
## Prompt enhancement

prompt.enhance accepts an installed LLM (llm-chat) model ID, a prompt string
(up to 64 KiB, empty allowed), and optional parameters. Execution runs in the
shared FIFO on text.chat's `server` engine with its default load settings
(`gpuLayers` all, `kvCacheLocation` gpu, `kvCacheType` f16, `flashAttention`
auto, the model's `contextLength`), so the model stays loaded between
enhancements and is shared with chats using those settings; a different model
or setting restarts it. Without llama-server, a llama-completion subprocess
runs instead. Errors keep the `prompt-enhancement-` codes on both. Recommendations default to the
standard LLM values: temperature 0.8, topP 0.95, contextLength 4096, maxTokens 1024.

```json
{"type":"submit","operation":"prompt.enhance","jobId":"enhance-1","options":{"modelId":"llm-chat:model.gguf","prompt":"a person","parameters":{"maxTokens":256,"systemPrompt":"Return JSON face and hair descriptions."}}}
```

Per-job overrides accept temperature, topP, contextLength and maxTokens with
standard LLM validation, string systemPrompt up to 64 KiB and boolean
penalizeRepeats (default true). false runs repetition penalty 1.0 over no tokens,
so faithful rewrites (translation, in-language improvement) can
reuse the input's words. Unknown keys fail.
Overrides never change model recommendations. Empty input requests a new
description using the system instructions. Temperature zero selects greedy output.
The subprocess uses repetition penalty 1.1 (unless penalizeRepeats is false). It passes no
top-k, min-p or sampler order, so these follow the GGUF's `general.sampling.*`
recommendations, or llama.cpp's defaults (top-k 40, min-p 0.05) when the file has none.
NUKLEUS_LLAMA_COMPLETION selects the executable. Preserve the ten-minute timeout,
1 MiB output limit, single-job queue, disconnect cancellation and explicit errors.

Success returns one JSON message without binary data:

```json
{"type":"result","operation":"prompt.enhance","jobId":"enhance-1","prompt":"..."}
```

## Text chat (additive v3 extension)

text.chat sends an ordered conversation to an installed LLM (llm-chat) and
returns one complete reply. It is generic: the service knows nothing about the
client's domain.

```json
{"type":"submit","operation":"text.chat","jobId":"chat-1","options":{"modelId":"llm-chat:model.gguf","messages":[{"role":"system","content":"You are Mia."},{"role":"user","content":"Hi."}],"parameters":{"maxTokens":512,"stop":["\nUser:"],"minP":0.05,"gpuLayers":"all","kvCacheLocation":"ram","kvCacheType":"q8_0"}}}
```

`messages` holds 1–1024 objects with exactly string `role` and `content`. A
`system` message may appear only first; other roles are `user` and
`assistant`; the last message is `user`. System and user content must not be
blank. The serialized messages total at most 1 MiB.

Optional `parameters`: `temperature`, `topP`, `contextLength` and `maxTokens`
(standard LLM validation, model recommendations as defaults);
`stop` (0–4 nonempty strings of at most 256 bytes); `topK` (integer 0–1000,
default 20); `minP` (0–1, default 0, disabled); `penalizeRepeats` (boolean,
default true) and `repeatPenalty` (1–2, default 1.1; `penalizeRepeats: false`
forces 1.0 over no tokens); `seed` (-1–2147483647, default -1, random);
`engine` (`server` or `completion`, default `server`); `gpuLayers` (`all`,
`auto` or an integer 0–999, default `all`); `kvCacheLocation` (`gpu` or
`ram`, default `gpu`); `kvCacheType` (`f16`, `q8_0` or `q4_0`, default
`f16`); `flashAttention` (`auto`, `on` or `off`, default `auto`); `jsonSchema`
(a JSON Schema object of at most 64 KiB; not combinable with `stop`);
`reasoning` (`off` or `on`, default `off`; reasoning text is always removed
from the reply, but its tokens count toward `maxTokens` and the turn's time);
DRY repetition control `dryMultiplier` (0–10, default 0, disabled), `dryBase`
(1–10, default 1.75), `dryAllowedLength` (integer 0–100, default 2) and
`dryPenaltyLastN` (integer -1–131072, default 64, -1 = whole context); XTC
`xtcProbability` (0–1, default 0, disabled) and `xtcThreshold` (0–1, default
0.1); `chatTemplate` (a llama.cpp built-in template name such as `chatml` or
`mistral-v7`, default: the model's own template). The service reads the
built-in names from the engine executable's `--help` and refuses others,
because llama.cpp would otherwise treat an unknown name as a literal Jinja
template. Unknown keys fail with `invalid-request`. Overrides never change model recommendations,
and none of these chat-only keys affect prompt.enhance or image jobs.

Load settings map to llama.cpp arguments: `gpuLayers` to `--n-gpu-layers`,
with `--fit on` for `auto` (fill the GPU, run the remaining layers on the CPU)
and `--fit off` otherwise, so a model that does not fit fails instead of
silently spilling; `kvCacheLocation: "ram"` to `--no-kv-offload`;
`kvCacheType` to `--cache-type-k` and `--cache-type-v`; `flashAttention` to
`--flash-attn`; `chatTemplate` to `--chat-template`; `reasoning` to
`--reasoning`. Sampling uses the order penalties, DRY, top-k, top-p, min-p,
XTC, temperature.

Engines:

- `server` (default) runs a llama-server child on 127.0.0.1 with a random
  port and a private API-key file. The model stays loaded between turns and
  the processed conversation prefix is reused (`cache_prompt`). A request
  whose model, `contextLength`, `gpuLayers`, `kvCacheLocation`, `kvCacheType`,
  `flashAttention`, `chatTemplate` or `reasoning` differs from the loaded one
  restarts it. The service
  unloads it before any other job (image work, a `completion` chat; not
  prompt.enhance, which runs on it) and after an idle period: ten minutes, or
  `NUKLEUS_LLAMA_SERVER_IDLE_SECONDS`. `NUKLEUS_LLAMA_SERVER` selects the
  executable.
- `completion` runs llama-completion through the shared FIFO like
  prompt.enhance and reloads the model on every turn: the system content opens
  the single user turn, followed by the conversation as `User:`/`Assistant:`
  lines.

Both engines share the service FIFO, so a chat waits behind queued image work.
`capabilities.operations` includes `text.chat` when an engine is ready, and
`capabilities.textChatEngines` lists the ready engines; a request for another
engine fails with `backend-not-ready`.

```json
{"type":"result","operation":"text.chat","jobId":"chat-1","text":"*smiles* Hello.","engine":"server","finishReason":"stop","usage":{"promptTokens":1834,"completionTokens":212},"durationMs":2140,"coldStart":false}
```

The reply is trimmed, `<think>` blocks are removed (an unclosed one is cut)
and the text is cut at the first `stop` string. `durationMs` is the whole
turn measured by the service, model loading included, and `coldStart` is true
when the turn loaded the model (always for `completion`). `finishReason`
(`stop` or `length`) and `usage` are reported by `server` only.

Structured replies: with `jsonSchema`, llama.cpp constrains generation to the
schema (`response_format` for `server`, `--json-schema-file` for
`completion`). The service checks that the cleaned reply parses as one JSON
value and adds it decoded as `json`; `text` keeps the raw JSON. A reply that
does not parse, typically one cut by `maxTokens`, fails with
`chat-invalid-json`. The schema is the client's: the service never interprets
the fields.

```json
{"type":"result","operation":"text.chat","jobId":"chat-2","text":"{\"speech\":\"*smiles* Hello.\"}","json":{"speech":"*smiles* Hello."},"engine":"server","finishReason":"stop","usage":{"promptTokens":1834,"completionTokens":12},"durationMs":640,"coldStart":false}
```

Failures: `chat-context-exceeded` (`server`: the conversation does not fit
`contextLength`; the message gives both token counts), `chat-failed` (with the
backend's trailing output, for example a model that does not fit in memory),
`chat-timeout` (ten minutes per turn, loading included), `chat-output-too-large`
(1 MiB), `chat-empty-result` and `chat-invalid-json`. Disconnecting cancels queued and active chats;
an active `server` chat kills llama-server. SDK clients use `canChat()`,
`submitChat(TextChatRequest)` and `chatFinished(TextChatResult)`.

### Images

An LLM whose `mmprojModelId` names an installed MMPROJ companion can see
images. On the `server` engine the service always loads it with that projector
(`--mmproj`), for chats with or without images and for prompt.enhance, so they
share the loaded model. Optional `imageCount` (integer 1–8, in `options` next to
`messages`) attaches images to the last user message:

```json
{"type":"submit","operation":"text.chat","jobId":"chat-3","options":{"modelId":"llm-chat:vision.gguf","imageCount":2,"messages":[{"role":"user","content":"What changed between these images?"}]}}
{"type":"upload-required","jobId":"chat-3","inputRole":"image","inputIndex":0}
```

The service sends one `upload-required` per image (`inputRole` `image`,
`inputIndex` 0…n-1) and the client answers each with one binary frame holding a
PNG or JPEG file; the chat is queued once all have arrived. llama-server
receives them in order, as `image_url` data URLs before the message's text.
Errors: `invalid-request` for images on the `completion` engine or an
`imageCount` out of range, `vision-unavailable` for an LLM without a usable
projector, `invalid-image` for an empty, oversized, or non-PNG/JPEG upload.
`capabilities.operations` includes `text.chat.images` when the `server` engine
is ready. SDK clients set `TextChatRequest::images` and check
`canChatWithImages(modelId)`; the SDK answers each upload itself.

### Loading and unloading the chat model

The `server` engine loads its model on the first chat that needs it. Clients
may also control it explicitly; both operations go through the service FIFO,
so they never interrupt a running job.

```json
{"type":"submit","operation":"text.chat.load","jobId":"load-1","options":{"modelId":"llm-chat:model.gguf","parameters":{"gpuLayers":"all","kvCacheType":"q8_0"}}}
{"type":"result","operation":"text.chat.load","jobId":"load-1","coldStart":true,"durationMs":8150}
{"type":"submit","operation":"text.chat.unload","jobId":"unload-1"}
{"type":"result","operation":"text.chat.unload","jobId":"unload-1","unloaded":true,"durationMs":310}
```

- `text.chat.load` takes the same `modelId` and `parameters` as `text.chat`,
  without `messages`, so a client can send its chat parameters unchanged; only
  the load settings (model, `contextLength`, `gpuLayers`, `kvCacheLocation`,
  `kvCacheType`, `flashAttention`, `chatTemplate`, `reasoning`) are used.
  `engine` must be `server` or absent. It succeeds once llama-server is ready;
  `coldStart` is false when the model was already loaded with those settings.
  The next chat with the same settings then starts warm. Load failures use the
  chat error codes, for example `chat-failed` for a model that does not fit.
- `text.chat.unload` takes no options and stops llama-server; `unloaded` tells
  whether a model was loaded.
- A loaded model still unloads before any other job and after the idle period.
- `capabilities.operations` lists both when the `server` engine is ready. SDK
  clients use `loadChatModel(modelId, parameters)`, `unloadChatModel()` and
  `chatModelFinished(ChatModelResult)`.

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
| image-generation | `width` and `height`: identical integers 256–2048, multiples of 256; `steps`: integer 1–200; `cfgScale`: number 0–100; `sampler` and `scheduler`: allowed strings; `clipSkip`: -1 or integer 1–12; optional `promptFormat`: `natural`, `phrases` or `tags` (default `phrases`) |
| llm-chat | `temperature`: number 0–2; `topP`: number >0 and ≤1; `contextLength`: integer 256–131072; `maxTokens`: integer 1–contextLength; optional booleans `canBeTextEncoder`, `defaultPromptEnhancer`, `defaultPromptTranslator` and `defaultImageDescriber` (default false) |
| upscaler | No options: empty object only. No explicit-content classification. |
| companion | `subtype`: exactly `vae`, `mmproj` or `t5xxl`; MMPROJ requires GGUF; T5-XXL requires GGUF or safetensors. |
| animation | Required for new installs: `vaeModelId` (an installed `vae` companion) and `textEncoderModelId` (an installed `t5xxl` companion), both nonblank strings; plus any subset of the [image.animate option table](#animating-an-image-imageanimate) as model defaults, validated as in requests. Unlike other recommendations, these defaults are applied to `image.animate` jobs. |

Samplers: empty (Automatic), `euler`, `euler_a`, `heun`, `dpm2`, `dpm++2m`, `lcm`.
Schedulers: empty (Automatic), `discrete`, `karras`, `exponential`, `ays`,
`sgm_uniform`, `simple`. Clip Skip -1 means Automatic. The GUI proposes defaults
from the shared Protocol API; the service does not infer model architecture.
`promptFormat` tells clients how the model reads prompts: natural sentences
(Flux), short comma-separated phrases (SDXL) or Danbooru-style tags
(Illustrious). It is set manually and never changes execution; SDK
`ModelInfo::promptFormat()` returns `phrases` when it is absent.
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
After a successful configure the service broadcasts `models.changed` with `id`
to the other authenticated clients; SDK/Manager refresh the catalogue on it.

At most one LLM carries each of `defaultPromptEnhancer: true`,
`defaultPromptTranslator: true` and `defaultImageDescriber: true`; the same model
may hold several. Installing or configuring an LLM with a role set clears that
role on every other LLM in the same atomic metadata write. Unchecking leaves the
role unassigned. The SDK exposes `defaultPromptEnhancerModelId()`,
`defaultPromptTranslatorModelId()` and `defaultImageDescriberModelId()` (empty
when unassigned). The service runs no job by role: clients choose the model.
The image describer is meant for an LLM with an MMPROJ companion, which the
service does not require when saving the role.

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
| inputCount | Optional integer 1–8 of reference images; not allowed with `hasInput: true` |

With `inputCount`, the service sends one `upload-required` per reference image
(`inputIndex` 0…n-1, role `reference`); the client answers each with one binary
frame in order, and sd-cli receives them as repeated `-r` arguments. Only models
with `canEdit` (missing means true) understand them; for any other model the service
rejects `inputCount` with `invalid-request`. SDK clients fill
`ImageGenerationRequest::referenceImages`.

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

Image-generation, LLM and animation objects in model lists and install/configure completion
messages expose explicitContent, default false. It is separate from recommendations.
Upscaler and Companion objects omit this field.

For Image generation, LLM and Animation, `models.install` and `models.configure` accept optional `explicitContent`.
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

`capabilities.operations` includes `image.edit` when the image backend is ready. Submit an installed Image-generation `modelId`, nonblank `prompt`, source `width`/`height`, and generation options through `submit` with operation `image.edit`. The service requires source bytes through the `upload-required` binary upload flow, even if `hasInput` is omitted. Optional `inputCount` (1–8, default 1) sets the number of images: the service sends one `upload-required` per image (`inputIndex` 0…n-1; role `source` for index 0, `reference` otherwise) and the client answers each with one binary frame in order. The first image is the one to edit; all are passed to sd-cli as repeated `-r` arguments in that order. Validation, queue/progress, result bytes and job errors follow the generation lifecycle. Results identify `image.edit`. Optional installed LLM, VAE and mmproj companions are resolved by the daemon; clients never supply paths. SDK clients use `submitEdit(ImageGenerationRequest)` and `ServiceCapabilities::imageEdit`.

### Preset routing

Each `model-routing` preset (routing document version 6) carries an integer
`identityViews` of 1, 2 or 4: the number of identity portraits clients request
sequentially with that preset. The service only stores and validates it;
documents of versions 1–4 load with 1.

An `image.generate` with a `presetId` is routed: the service uses the preset's
assigned model and fills omitted `steps`/`cfgScale` from its saved snapshot. An
unassigned preset fails with `model-routing-unavailable`. Presets have no
quality tiers; `image.generate` rejects `qualityId`.

The routing document's `editingQualities` array holds the fixed `edit-low`
(Fast), `edit-medium` (Balanced) and `edit-high` (Quality) entries, each with
`id`, `name`, `nameFr`, `modelId`, optional `steps`/`cfgScale`, a boolean
`default` and `positivePrefix`/`negativePrefix` strings (empty when absent); exactly one is the default, which clients preselect and use when the
user makes no quality choice. An `image.edit` with a `qualityId` is routed: the
service uses that quality's model and fills omitted `steps`/`cfgScale` from its
snapshot. After validating the original `prompt`, it places the quality's
prefixes first (comma-separated) in the prompt and negative prompt passed to
sd-cli. Prefixes never pass through an LLM and results never include them. It rejects `presetId`, `styleId` and a conflicting `modelId` with
`invalid-request`; an unknown or unassigned quality fails with
`model-routing-unavailable`. An `image.edit` without `qualityId` uses its
`modelId` directly.

The routed generation `result` message carries `prompt` and `negativePrompt`:
the submitted (trimmed) prompts without preset prefixes. SDK clients read
`ImageJobResult::prompt`/`negativePrompt`.

Presets have no prompt flows: a routed generation never runs an LLM, and there is
no `prompt-flow.run` operation or `skipPromptFlow` option (an unknown option fails
with `invalid-request`). Routing documents saved with a `promptFlow` still load;
it is ignored and no longer written. Clients translate or improve prompts
themselves through `prompt.enhance` with the LLMs marked `defaultPromptTranslator`
and `defaultPromptEnhancer`.



## Local identity portrait packages

IdentityPackage is a local SDK archive helper, not a WebSocket operation. It stores
manifest.json (type kira-identity, version 2), one to four ordered PNG references,
face/hair prompts, seed metadata and optional generationPrompt/editPrompt strings
telling generation and editing how to use the references. No binary conditioning or extraction upload
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

Standalone Companion files live in models/companion/, with VAE, MMPROJ or T5-XXL subtype
stored under recommendedOptions.subtype. VAE accepts safetensors/ckpt/GGUF;
MMPROJ accepts GGUF; T5-XXL (animation text encoders) accepts GGUF/safetensors. LLM install/configure accepts optional string mmprojModelId.
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

## Animation models

The `animation` category keeps image-to-video models (such as Wan 2.2 I2V A14B)
with every file they need. Weights live in `models/animation/` (split layout:
`models-dir/animation/`) and the catalogue in `animation/metadata.json` (split
layout: `catalog/animation.json`), in the same per-category metadata format.
The primary file is the low-noise diffusion model (GGUF or safetensors); the
model type is always `diffusion`, and `modelType` other than `diffusion` is
rejected.

| Role | Kind | Required | Stored as |
| --- | --- | --- | --- |
| `primary` | primary file | yes | `animation/<file>` |
| `high-noise` | per-model attachment | no | `companions/<primary>/high-noise/<file>` |
| `lora-low-noise` | per-model attachment | no | `companions/<primary>/lora/<file>` |
| `lora-high-noise` | per-model attachment | no | `companions/<primary>/lora/<file>` |
| `vae` | shared companion, `recommendedOptions.vaeModelId` | yes | Companion of subtype `vae` |
| `text-encoder` | shared companion, `recommendedOptions.textEncoderModelId` | yes | Companion of subtype `t5xxl` |

Attachments are GGUF or safetensors files. Both LoRA stages share one `lora`
directory, so two LoRAs of one model need distinct file names.

A new model is installed in one `models.install` with a display name, the
diffusion model `url`, optional `companionUrls` for the attachment roles, and
`recommendedOptions` naming the installed VAE and T5-XXL companions. Progress
reports the `fileRole` of each file. The model is listed only after every file
and its metadata are saved; cancellation or failure removes every file the
installation staged or published and leaves other models unchanged.

```json
{"type":"models.install","category":"companion","installId":"t5","url":"https://example.com/umt5-xxl-encoder-Q8_0.gguf","displayName":"UMT5-XXL","recommendedOptions":{"subtype":"t5xxl"}}
{"type":"models.install","category":"animation","installId":"wan","url":"https://example.com/wan2.2_i2v_low_noise_14B_Q4_K_M.gguf","displayName":"Wan 2.2 I2V A14B","recommendedOptions":{"vaeModelId":"companion:wan_2.1_vae.safetensors","textEncoderModelId":"companion:umt5-xxl-encoder-Q8_0.gguf"},"companionUrls":{"high-noise":"https://example.com/wan2.2_i2v_high_noise_14B_Q4_K_M.gguf","lora-low-noise":"https://example.com/wan2.2_i2v_low_noise_lightx2v_4step.safetensors","lora-high-noise":"https://example.com/wan2.2_i2v_high_noise_lightx2v_4step.safetensors"}}
```

An optional file is attached later with the existing attachment install (`id`
of the model, `companionUrls`, no `url`): the primary is not downloaded again
and the name, recommendations and classification stay unchanged unless sent.
A role whose file is missing can receive a new file; a role whose file is
present must be removed first with `models.companion.delete`.

Animation entries in `models.list.result`, `models.install.completed` and
`models.configure.completed` carry `id`, `name`, `displayName`, `fileName`,
`bytes`, `explicitContent`, `recommendedOptions`, `companionFiles` (role to
category-relative file name) and `missingFiles`: the roles among `primary`,
`high-noise`, `lora-low-noise`, `lora-high-noise`, `vae` and `text-encoder` whose
file or shared companion is absent. A model whose primary file is missing stays
listed, with `bytes` 0, so that it can be reported and deleted. Paths are never
returned.

Deleting an animation model removes its primary and attachments and keeps the
shared VAE and text-encoder companions. A companion assigned to an animation
model, installed or being installed, cannot be deleted nor changed to another
subtype. A model used by queued or active work, or receiving files, cannot be
deleted nor lose a file. Display names and the explicit-content classification
follow the image-generation rules. SDK `ModelInfo` exposes
`ModelCategory::Animation` and `missingFiles`; `installModel` and
`configureModel` accept the category.

## Animating an image (image.animate)

`capabilities.operations` includes `image.animate` when the sd-cli backend is
ready. A client submits an installed animation model, a nonblank prompt and a
PNG or JPEG start image, and optionally an end image; the service runs
`sd-cli -M vid_gen` and returns a short video.

```json
{"type":"submit","operation":"image.animate","jobId":"anim-1","options":{"modelId":"animation:wan2.2_i2v_low_noise_14B_Q4_K_M.gguf","prompt":"she breathes slowly","seed":-1,"hasEndImage":false,"parameters":{"videoFrames":41,"steps":4,"highNoiseSteps":4,"cfgScale":1,"highNoiseCfgScale":1}}}
{"type":"upload-required","jobId":"anim-1","inputRole":"start","inputIndex":0}
{"type":"queued","jobId":"anim-1"}
{"type":"running","jobId":"anim-1"}
{"type":"progress","jobId":"anim-1","progress":{"stage":"sampling","currentStep":5,"totalSteps":8,"percentage":61,"statusText":"Sampling video 5 / 8"}}
{"type":"result","jobId":"anim-1","operation":"image.animate","mimeType":"video/webm","frames":41,"fps":16}
```

The result message is followed by one binary frame holding the video.

**Request.** `options` holds `modelId` (an installed `animation` model),
`prompt` (required, nonblank), `negativePrompt` (string), `seed` (integer -1 to
2147483647, default -1, random), `hasEndImage` (boolean, default false) and
`parameters` (object). Every other option sits in `parameters`. A prompt or
negative prompt containing `<lora:` is rejected: the model's LoRAs are applied
by the service (below). An unknown key, a wrong type or an out-of-range value
fails with `invalid-request` before any upload is requested; a model whose
`missingFiles` is not empty fails with `model-files-missing`, and an unknown
model with `model-unavailable`. `pingPong` true, from the request or the
model's defaults, fails with `backend-not-ready` before any upload when the
service has no `ffmpeg` (below); `container` `mp4` fails with
`backend-not-ready` until it is supported.

**Uploads.** The service sends `upload-required` with `inputRole` `start` and
`inputIndex` 0, then, with `hasEndImage` true, `inputRole` `end` and `inputIndex`
1. Each answer is one binary frame holding a PNG or JPEG image; any other format
fails with `invalid-image`. The start image is passed as `-i`, the end image as
`--end-img`.

**Options.** The `highNoise…` options apply to the high-noise pass; the others
to the low-noise (main) pass. The table lives in the Protocol target
(`Nukleus::Protocol::animationOptions()`), shared by request validation and
model defaults.

| Group | Option | sd-cli flag | Accepted values |
| --- | --- | --- | --- |
| Video | `width`, `height` | `-W`, `-H` | integers 64–4096, multiples of 16 |
| Video | `videoFrames` | `--video-frames` | integer 1–1024 |
| Video | `fps` | `--fps` | integer 1–120 |
| Video | `container` | output extension | `webm` (default), `webp`, `avi`, `mp4` (not available yet) |
| Video | `compressionQuality` | `--compression-quality` | integer 0–100 |
| Video | `pingPong` | none (post-processing with `ffmpeg`) | boolean; true needs `image.animate.pingPong` |
| Sampling | `steps`, `highNoiseSteps` | `--steps`, `--high-noise-steps` | integer 1–200; `highNoiseSteps` also -1 (automatic split) |
| Sampling | `cfgScale`, `highNoiseCfgScale` | `--cfg-scale`, `--high-noise-cfg-scale` | finite number 0–100 |
| Sampling | `imgCfgScale`, `highNoiseImgCfgScale` | `--img-cfg-scale`, `--high-noise-img-cfg-scale` | finite number 0–100 |
| Sampling | `guidance`, `highNoiseGuidance` | `--guidance`, `--high-noise-guidance` | finite number 0–100 |
| Sampling | `sampler`, `highNoiseSampler` | `--sampling-method`, `--high-noise-sampling-method` | a method listed by sd-cli's `--help` |
| Sampling | `scheduler` | `--scheduler` | a scheduler listed by sd-cli's `--help` |
| Sampling | `sigmas` | `--sigmas` | comma-separated finite numbers, or a preset listed by `--help` |
| Sampling | `eta`, `highNoiseEta` | `--eta`, `--high-noise-eta` | finite number 0–10 |
| Sampling | `flowShift` | `--flow-shift` | finite number 0–100 |
| Sampling | `moeBoundary` | `--moe-boundary` | finite number 0–1 |
| Sampling | `strength` | `--strength` | finite number 0–1 |
| Sampling | `slgScale`, `highNoiseSlgScale` | `--slg-scale`, `--high-noise-slg-scale` | finite number 0–100 |
| Sampling | `skipLayers`, `highNoiseSkipLayers` | `--skip-layers`, `--high-noise-skip-layers` | array of 1–64 integers 0–255 |
| Sampling | `skipLayerStart`, `skipLayerEnd`, `highNoiseSkipLayerStart`, `highNoiseSkipLayerEnd` | matching `--…skip-layer-start/end` | finite number 0–1 |
| Sampling | `extraSampleArgs` | `--extra-sample-args` | key=value list |
| Sampling | `rng`, `samplerRng` | `--rng`, `--sampler-rng` | `std_default`, `cuda`, `cpu` |
| Sampling | `prediction` | `--prediction` | a type listed by `--help` |
| LoRA | `loraWeights` | `<lora:…:W>` tags | object with optional `lowNoise`, `highNoise`: finite numbers -10–10 |
| LoRA | `loraApplyMode` | `--lora-apply-mode` | `auto`, `immediately`, `at_runtime` |
| Cache | `cacheMode` | `--cache-mode` | `easycache`, `ucache`, `dbcache`, `taylorseer`, `cache-dit`, `spectrum` |
| Cache | `cacheOption` | `--cache-option` | key=value list |
| Cache | `scmMask`, `scmPolicy` | `--scm-mask`, `--scm-policy` | comma-separated 0/1; `dynamic` or `static` |
| Memory | `offloadToCpu`, `mmap`, `eagerLoad`, `disablePrefetch`, `disableSegmentedCompute` | matching flags | boolean (flag passed when true) |
| Memory | `flashAttention`, `convDirect` | `--flash-attn`, `--conv-direct` | `on`, `off`, `auto` or module assignments |
| Memory | `autoFit`, `batchedCfg` | `--auto-fit`, `--batched-cfg` | `on`, `off` |
| Memory | `maxVram` | `--max-vram` | number of GiB (-1024–1024) or device assignments |
| Memory | `backend`, `paramsBackend`, `splitMode` | `--backend`, `--params-backend`, `--split-mode` | device or module assignments |
| Memory | `threads` | `-t` | integer -1–1024 |
| Memory | `conditioningCacheSize` | `--conditioning-cache-size` | integer 0–64 |
| Memory | `weightType` | `--type` | a type listed by `--help` |
| Memory | `vaeFormat` | `--vae-format` | `auto`, `flux`, `sd3`, `flux2`, `wan` |
| Memory | `linearScale`, `attnScale` | `--linear-scale`, `--attn-scale` | finite number 0–100 |
| VAE tiling | `vaeTiling`, `temporalTiling` | `--vae-tiling`, `--temporal-tiling` | boolean (flag passed when true) |
| VAE tiling | `vaeTileSize`, `vaeRelativeTileSize` | `--vae-tile-size`, `--vae-relative-tile-size` | `S` or `WxH` |
| VAE tiling | `vaeTileOverlap` | `--vae-tile-overlap` | finite number 0–1 |
| VAE tiling | `extraTilingArgs` | `--extra-tiling-args` | key=value list |

Free-form strings (key=value lists, assignments, sizes) are at most 256 bytes of
`[A-Za-z0-9_.,=:&+-]`, so none can name a file or a network endpoint; each is
passed as one argument. Names listed by sd-cli's `--help` are checked against
the installed sd-cli, read once by the service. Options that name files or
endpoints, or belong to other sd-cli modes, are not offered.

**Defaults.** For each option absent from the request, the service uses the
model's value from `recommendedOptions`; when the model has none either, the
sd-cli flag is omitted so that sd-cli's own default applies. Only `container`
(`webm`) and `seed` (-1) have service defaults. This is an exception, for
`image.animate` only, to the rule that recommended options are never applied to
jobs. `loraWeights` resolves per stage.

**Files and LoRAs.** The service passes the model's files (`--diffusion-model`,
`--high-noise-diffusion-model` when attached), its shared companions (`--vae`,
`--t5xxl`) and, when a LoRA applies, `--lora-model-dir`. Each attached LoRA
applies to its stage with the weight from the request's `loraWeights`, else the
model's, else 1: the service appends `<lora:NAME:W>` (low-noise) or
`<lora:|high_noise|NAME:W>` (high-noise) to the prompt it passes to sd-cli,
where NAME is the LoRA file name without its extension. A weight of 0 leaves
the LoRA out. Results never echo the tags.

**Ping-pong loops.** With `pingPong` true, from the request or the model,
the service runs `ffmpeg` on sd-cli's video once sd-cli succeeds, and returns a
loop that plays the generated frames forward then backward without repeating
the last frame or ending on the first: N generated frames become 2N - 2, in the
requested container and at the same fps (WebM as VP9, animated WebP, AVI as
MJPEG). `capabilities.operations` includes `image.animate.pingPong` only when
the service's `ffmpeg` runs: it is found on the service's PATH, or named by
`NUKLEUS_FFMPEG`, and must answer `ffmpeg -version`. A failure returns
`generation-failed` with the ffmpeg command, its exit code and up to 4096
trailing stderr characters.

```json
{"type":"progress","jobId":"anim-1","progress":{"stage":"decoding","percentage":96,"statusText":"Creating loop…"}}
{"type":"result","jobId":"anim-1","operation":"image.animate","mimeType":"video/webm","frames":80,"fps":16}
```

**Queue, progress and cancellation.** Animation jobs share the FIFO with image
jobs (`queued`, `queue-position`, `running`) and unload llama-server first.
`progress` messages follow the run (`preparing`, `starting`, `loading-model`,
`sampling`, `decoding`, `saving`, `complete`). The sampling step counts both
noise passes, from 1 to their total (`steps` plus `highNoiseSteps` when both
are set, `steps` split between the passes when `highNoiseSteps` is -1, the
numeric `sigmas` count minus one), with a percentage that never decreases;
without a `steps` option, sd-cli's default from `--help` is used. A ping-pong
job reports `Creating loop…` (stage `decoding`) between decoding and saving.
When the submitting client disconnects, its queued animations are dropped and
its active one is stopped, sd-cli or ffmpeg alike, as for image jobs; there is
no per-job cancel message.

**Result.** `result` carries `operation` `image.animate`, `mimeType`
(`video/webm`, `image/webp` or `video/x-msvideo`), `frames` (the frame count of
the returned video: the count sd-cli generated, which it may align to the model,
or the loop's count with `pingPong`) and `fps` (the option, else
sd-cli's default from `--help`), followed by one binary frame. A failure returns
`generation-failed` with the sd-cli command, its exit code and up to 4096
trailing stderr characters, as for image generation.

**SDK.** `ServiceClient::canAnimate()` reports the capability
(`ServiceCapabilities::imageAnimate`), and `canPingPong()` the ping-pong one
(`ServiceCapabilities::imageAnimatePingPong`). `submitAnimation(AnimationRequest)` sends
the model ID, prompts, seed, start image, optional end image and a `QJsonObject`
of parameters, and uploads the images when asked. Progress arrives through
`generationProgressChanged`; `animationFinished(AnimationResult)` carries the
video bytes, `mimeType`, `frames` and `fps`, or the error code and message.
