# Kira Service WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The version-3 authentication message is required for every client. Loopback peers may send an empty token. Non-loopback peers must send the configured shared LAN token.

The service replies with `authenticated` and `capabilities`. `models.list` returns the `image-generation`, `edition`, `upscaler`, `llm-chat`, and `ip-adapter` categories. Each model contains `id`, `name`, `fileName`, and `bytes`; a model with a companion also includes the category-relative `companionFileName` and `companionRole` (`lora`, `mmproj`, or `vae`).

Model administrators send `models.install` with a client-generated `installId`, explicit category, and HTTPS model-file URL. Optional `companionUrl` is a LoRA for `ip-adapter`, mmproj for `llm-chat`, or VAE for `image-generation` and `edition`; it is rejected for `upscaler`. mmproj requires a `.gguf` filename; VAE and LoRA accept `.safetensors`, `.ckpt`, or `.gguf`. SHA-256 is no longer checked or accepted by the SDK/manager; legacy protocol `sha256` fields are ignored. The service stages both downloads and commits the primary, companion, and relationship metadata together, so a failed or canceled pair leaves no usable catalog entry. A restart preserves the relationship; deletion removes both files. Listings and install completion expose the companion filename and role, never absolute paths. Image-generation VAE companions are resolved internally and passed to sd-cli with --vae; other companion roles do not alter image invocation. The service emits `models.install.accepted`, `models.install.progress`, `models.install.completed`, or an `error` containing that `installId`.

For example:

```json
{"type":"models.install","installId":"adapter-1","category":"ip-adapter","url":"https://huggingface.co/h94/IP-Adapter-FaceID/resolve/main/ip-adapter-faceid-plusv2_sdxl.bin?utm_source=chatgpt.com","companionUrl":"https://huggingface.co/h94/IP-Adapter-FaceID/resolve/main/ip-adapter-faceid-plusv2_sdxl_lora.safetensors?utm_source=chatgpt.com"}
```

Model administrators may delete an installed model by its opaque ID:

```json
{"type":"models.delete","id":"upscaler:model.gguf"}
```

The service replies with `models.delete.completed` or an `error` containing the model `id`. Deletion is rejected while the model is referenced by active, queued, or pending-upload work. Filesystem paths are never accepted or returned.

Image work uses `submit` with `image.generate` or `image.upscale`; each request must include a matching installed `modelId`. Upscale is fixed at 2×. Input and result image data are binary WebSocket messages; filesystem paths are never part of the protocol.

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
| ip-adapter | `strength`: number 0–2 |

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
| width, height | Required integers 64–4096; rectangular sizes are allowed |
| steps | Required integer 1–200 |
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

## Optional image model files (additive v3 extension)

Image-generation and Edition models expose `modelType` (`checkpoint` by default,
`diffusion` for standalone UNets) and `companionFiles`, mapping `clip-l`, `clip-g`
and `vae` to category-relative companion filenames. Absolute paths are never
accepted or returned. The legacy single-companion fields remain compatible.

`models.install` accepts optional `companionUrls` mapping these roles to HTTPS
URLs, plus `modelType`. A request with an existing model `id` attaches missing
roles without downloading its primary or changing recommendations. Omit
`recommendedOptions` to preserve them. An omitted/empty modelType preserves the
existing type when attaching; new installations default to checkpoint. Duplicate roles, unsupported
roles, invalid URLs, existing destination files, and models in use are rejected.
Downloads use the existing install progress, owned cancellation, and completion
messages; `fileRole` may now be `clip-l`, `clip-g` or `vae` as well as legacy
`primary`/`companion`. Failures remove newly staged/published files, preserving
pre-existing model files and metadata. A role's presence means a file is attached,
not that arbitrary components are mutually compatible.

```json
{"type":"models.install","installId":"attach-123","category":"image-generation","id":"image-generation:existing.gguf","modelType":"diffusion","companionUrls":{"clip-l":"https://example.com/clip_l.safetensors","clip-g":"https://example.com/clip_g.safetensors","vae":"https://example.com/vae.safetensors"}}
```

The daemon snapshots attached file paths for each image job. Only registered
files are passed to the backend; neither folder scanning nor filename/extension
heuristics determine model type. Unused companions remain optional.
