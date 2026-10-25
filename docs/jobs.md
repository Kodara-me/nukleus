# Jobs

How the service runs the jobs clients submit. All jobs share one FIFO queue.
The wire messages are in [protocol-v3.md](protocol-v3.md); the models they use
are in [Models](models.md). Image work and text work need the runtime tools
listed under [Runtime tools on Arch Linux](../README.md#runtime-tools-on-arch-linux).

## Image generation

Generation requests remain caller-controlled: recommendedOptions are never applied
implicitly, except for `image.animate`, which uses an animation model's defaults for the
options a request omits (see [Animating an image](#animating-an-image)). The daemon validates option types/ranges before queueing, omits
Automatic sampler/scheduler flags and preserves CFG precision. Installed VAE
companions are resolved on the server and passed to both text-to-image and
image-to-image jobs without exposing filesystem paths.
Omitted dimensions and steps default to 1024 × 1024 and 28; CFG defaults to 7.
No external VAE is required for a complete checkpoint with an embedded VAE.
Standalone diffusion files require their missing encoder/VAE weights separately.

Generation failures include the attempted sd-cli command (executable and quoted
arguments), exit code and up to 4096 trailing stderr characters in the job error
message. This diagnostic intentionally exposes the submitting client's prompts
and service-side model/output paths to that client only; successful results and
model listings continue to omit service paths. Commands are displayed as POSIX
shell-quoted text for diagnosis; execution still uses QProcess argument lists.

Image generation and reference-image editing both pass `--offload-to-cpu` to sd-cli.
For temporary testing, all image generation and editing calls also
pass `--diffusion-fa` exactly once. They also pass `--vae-conv-direct`, which
decodes with direct convolutions instead of oversized im2col buffers that exceed
the Vulkan pinned-memory limit. Per-model configuration is deferred.

## Image editing

Image editing uses the additive protocol-v3 `image.edit` operation and capability.
The SDK exposes `submitEdit(ImageGenerationRequest)` with nonempty instructions
and source image bytes. The daemon resolves an image-generation model and its optional
LLM, VAE and mmproj companions, queues the uploaded reference, and runs sd-cli
with `--ref-image`, `--prompt`, requested width/height and generation parameters.
Companions use `--llm`, `--vae` and `--llm_vision`; primary weights use
`--diffusion-model` or `--model` according to saved modelType. Reference presets
are auto-detected by sd-cli; no img2img `--init-img` or strength is supplied.
See [upstream editing documentation](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/edit.md).

Reference-image editing also supplies `--flow-shift 3 --offload-to-cpu --diffusion-fa --vae-conv-direct --model-args qwen_image_zero_cond_t=true`. Fatal/error-level Vulkan `ErrorOutOfDeviceMemory`/`ErrorOutOfHostMemory` output aborts the active generation process; the daemon waits for process termination and returns the command and memory diagnostic as a job error, releasing the queue. Warnings, including pinned-memory allocation failures that fall back to CPU buffers, are not treated as failures.

## Upscaling

Upscaler models run a fixed 2× upscale and have no options. The SDK supports
fixed 2× upscaling and model discovery next to image generation, progress and
binary results.

## Prompt enhancement

Prompt enhancement uses the **LLM** catalogue (llm-chat, stored under models/llm/).
A client can select its default LLM model with the LLM roles described in
[Models](models.md#recommended-options).
prompt.enhance runs on the same loopback `llama-server` as text.chat, with the
model's temperature, top-p, context length and maximum tokens. The model stays
loaded between enhancements and is shared with chats that use text.chat's
default load settings; another model or setting unloads it and loads the new
one, and image jobs unload it first. Without `llama-server`, a fresh
`llama-completion` process runs each request. The identity-specific system
prompt is a per-job override. Model settings are unchanged by individual requests.

Install llama.cpp's `llama-server` (or `llama-completion`) executable on the
service host to enable this capability. It must be on the daemon's PATH;
alternatively set `NUKLEUS_LLAMA_SERVER` or `NUKLEUS_LLAMA_COMPLETION` to its
absolute path in the service environment.

## Text chat

text.chat sends a client-built conversation (system, user and assistant
messages) to an installed LLM and returns one complete reply. It is generic:
the service knows nothing about the client's domain. Its default `server`
engine runs llama.cpp's `llama-server` on loopback and keeps the model loaded
between turns; the `completion` engine reuses `llama-completion` and reloads
the model on every turn. Clients choose the engine, sampling, GPU layer count
and KV-cache placement per request. `llama-server` starts on the first chat
that needs it, or earlier through `text.chat.load`; `text.chat.unload` stops
it. The service also unloads it before any other job and after ten minutes
without a chat. Set `NUKLEUS_LLAMA_SERVER` to
override the executable and `NUKLEUS_LLAMA_SERVER_IDLE_SECONDS` to change the
idle period, for example in `/etc/nukleus/nukleus.conf`. See
[`protocol-v3.md`](protocol-v3.md).

## Animating an image

`image.animate` turns a PNG or JPEG start image, and optionally an end image,
into a short video with an installed animation model, through
`sd-cli -M vid_gen`. A request carries the model ID, a prompt, an optional
negative prompt and seed, and a `parameters` object holding any sd-cli video
option of the [option table](protocol-v3.md#animating-an-image-imageanimate):
size, frame count, fps, container (`webm`, `webp`, `avi`, `mp4`), steps, guidance and
sampler for each noise pass, flow shift, LoRA weights, cache, memory and VAE
tiling options. Each option the request leaves out comes from the model's
`recommendedOptions`, else is left to sd-cli's own default; this is the one
operation that applies recommendations to jobs. The model's LoRAs are added to
the prompt by the service with their stage and weight (`loraWeights`, default 1,
0 leaves one out). Jobs share the FIFO with image jobs, report the same
`progress` messages with the sampling step counted across both noise passes,
and stop when their client disconnects. The result is the video bytes with its
MIME type, frame count and fps. SDK clients use `canAnimate()`,
`submitAnimation(AnimationRequest)` and `animationFinished(AnimationResult)`.

With `pingPong` true (from the request or the model's defaults), the service
post-processes the video with `ffmpeg` into a seamless loop that plays the
frames forward then backward without repeating the turning frames, in the same
container and fps; `frames` reports the loop's count (2N - 2). `ffmpeg` is an
optional runtime tool, like `llama-server`: it is found on the service's PATH or
named by `NUKLEUS_FFMPEG`, and `image.animate.pingPong` is listed in the
capabilities (SDK `canPingPong()`) only when it runs; without it, `pingPong`
fails with `backend-not-ready` before any upload.

`container` `mp4` plays everywhere (phones, browsers, players, game engines):
sd-cli writes AVI and the same `ffmpeg` encodes it to H.264 (`yuv420p`, index at
the start of the file) at the same fps, in one pass with the ping-pong loop when
`pingPong` is true. `image.animate.mp4` is listed in the capabilities (SDK
`canAnimateMp4()`) only when `ffmpeg` runs; without it, `mp4` fails with
`backend-not-ready` before any upload. `compressionQuality` (0–100) goes to
sd-cli and sets the quality of every `ffmpeg` encode too, loops and MP4 alike
(90 when absent). Before returning or encoding sd-cli's file, the service checks
that its content is the container requested (a sd-cli built without libwebm
writes AVI under a `.webm` name) and fails with `generation-failed` naming both
otherwise, so `mimeType` always matches the file.
