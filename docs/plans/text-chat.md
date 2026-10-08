---
status: implemented
projects: [kira-service]
---

# Generic multi-turn text chat

## Status

Implemented in Service and SDK. Kira Meet is the first consumer, but nothing in
this plan is specific to it; Kira Meet integration is out of scope.

[`protocol-v3.md`](../protocol-v3.md) is the authoritative contract. The
implementation added chat-only sampling overrides beyond this plan (`topK`,
`minP`, `repeatPenalty`, `seed`, `flashAttention`, `reasoning`, DRY and XTC
values, a validated built-in `chatTemplate`), schema-constrained
structured replies (`jsonSchema`, the decoded `json` result and
`chat-invalid-json`), explicit `text.chat.load` and `text.chat.unload`
operations, `durationMs` and `coldStart` results, a private API-key file instead of `--api-key`, and the
`chat-canceled` code reported internally when a disconnect cancels a chat.

## Goal

Let a client send an ordered list of chat messages to an installed LLM and
receive one complete reply. The service knows nothing about the client's
domain: it formats, executes and returns text.

## Scope

Include a new protocol operation `text.chat`, two interchangeable execution
engines (a managed `llama-server` process and the existing `llama-completion`
subprocess) so they can be compared, GPU coexistence with image jobs, SDK
support, tests and documentation.

Exclude streaming (replies are always complete blocks), image input, tool
calls, server-side conversation storage, context truncation (the client
decides what to drop), a Manager "default chat model" role (clients choose the
model ID themselves) and any change to `prompt.enhance`.

## Existing Context

- `prompt.enhance` runs one `llama-completion` subprocess per job through the
  shared single-job FIFO, with a ten-minute timeout, a 1 MiB output limit,
  disconnect cancellation and explicit error codes.
- Instructions are prepended to the single user message because chat templates
  such as Qwen3.5 abort on a system prompt without a user query.
- `llm-chat` models carry recommended `temperature`, `topP`, `contextLength`
  and `maxTokens`, validated by `ModelOptions`, plus `explicitContent`.
- `llama-server` and `llama-completion` are both available on the service host.
- systemd runs the daemon with `ProtectHome` and `ProtectSystem=strict`; a
  loopback child listener is not restricted.

## Contracts and Interfaces

### Request

```json
{"type":"submit","operation":"text.chat","jobId":"chat-1","options":{
  "modelId":"llm-chat:model.gguf",
  "messages":[
    {"role":"system","content":"You are …"},
    {"role":"user","content":"Hi."},
    {"role":"assistant","content":"Hello."},
    {"role":"user","content":"*sits down* How was your day?"}
  ],
  "parameters":{"temperature":0.9,"maxTokens":512,"stop":["\nUser:"],"engine":"server"}
}}
```

- `messages`: 1–1024 objects with exactly `role` and `content` strings.
  `role` is `system`, `user` or `assistant`. A `system` message may appear only
  first. The last message must be `user`. Content may be empty only for
  `assistant`. The serialized messages total at most 1 MiB.
- `parameters` (optional): `temperature`, `topP`, `contextLength`, `maxTokens`
  with the existing LLM validation and model recommendations as defaults;
  `penalizeRepeats` (boolean, default true); `stop` (0–4 nonempty strings of at
  most 256 bytes); `engine` (`server` or `completion`, default `server`);
  `gpuLayers` (`all`, `auto`, or an integer 0–999, default `all`),
  `kvCacheLocation` (`gpu` or `ram`, default `gpu`) and `kvCacheType` (`f16`,
  `q8_0` or `q4_0`, default `f16`, applied to both K and V). See
  [Memory placement](#memory-placement). These three exist only for
  `text.chat`.
- Unknown keys anywhere fail with `invalid-request` before queueing.
  Overrides never change model metadata.

### Result

```json
{"type":"result","operation":"text.chat","jobId":"chat-1","text":"…","engine":"server",
 "finishReason":"stop","usage":{"promptTokens":1834,"completionTokens":212}}
```

- `text` is trimmed, with reasoning blocks (`<think>…</think>`) removed and
  output cut at the first `stop` string.
- `finishReason` is `stop` or `length`. `usage` and `finishReason` are present
  for the `server` engine and omitted for `completion`, which does not report
  them reliably.

### Errors

Existing codes keep their meaning (`invalid-request`, `model-unavailable`,
`llm-format-unsupported`, `backend-not-ready`, `queue-full`). New codes:
`chat-context-exceeded` (the prompt plus `maxTokens` does not fit
`contextLength`), `chat-timeout`, `chat-output-too-large`,
`chat-empty-result` and `chat-failed` (with up to 4096 trailing stderr or
server error characters).

### Capabilities

`capabilities.operations` includes `text.chat` when at least one engine is
ready. An additive `capabilities.textChatEngines` array lists the ready
engines (`server`, `completion`). A request for an unready engine fails with
`backend-not-ready`.

### SDK

- `TextChatMessage { role, content }`, `TextChatRequest { modelId, messages,
  parameters }`, `TextChatResult { jobId, succeeded, text, engine,
  finishReason, promptTokens, completionTokens, errorCode, message }`.
- `ServiceCapabilities::textChat` and `textChatEngines`.
- `ServiceClient::canChat(modelId)`, `submitChat(request)` returning the job
  ID, and the `chatFinished(TextChatResult)` signal. Queue state reuses
  `jobStateChanged`.

## Memory placement

Where the model and the KV cache (the per-token memory of the conversation)
live is not settled yet: the first target is a model of about 20 GB on a
24 GB GPU, and smaller models will be tried. The client therefore chooses the
placement on every request, so it can be changed between tests without
touching the service.

- **Model weights: `gpuLayers`.**
  - `all` (default) passes `--n-gpu-layers all`: every layer on the GPU. A
    model that does not fit fails to load with an explicit `chat-failed`
    error instead of silently slowing down.
  - `auto` passes `--n-gpu-layers auto --fit on`: llama.cpp fills the GPU,
    keeping its default 1 GiB margin, and runs the remaining layers on the
    CPU. Useful to test models slightly larger than the VRAM.
  - An integer passes that exact layer count, for fine-grained tests.
- **KV cache: chosen by the client.** `kvCacheLocation: "ram"` passes
  `--no-kv-offload`; `"gpu"` keeps llama.cpp's default. `kvCacheType` passes
  `--cache-type-k` and `--cache-type-v`; `q8_0` halves the cache size with a
  small quality cost.
- Trade-offs to measure: with the cache in RAM, attention runs on the CPU, so
  each generated token is slower, increasingly with conversation length; with
  `auto`, every layer left on the CPU slows generation too. The manual
  comparison covers these combinations as well as the two engines.
- These values are client choices, not model recommendations. They are not
  stored in model metadata and do not affect `prompt.enhance` or image jobs.

## Engines

### `server`: managed `llama-server`

A new `LlamaServerRunner` in `src/daemon/` owns one `llama-server` child:

- Launched on demand with `--model`, `--ctx-size`, the memory-placement
  arguments, `--host 127.0.0.1`, a free ephemeral `--port`, a random
  per-launch `--api-key`, `--no-webui`, `--parallel 1`, `--jinja` and
  `--reasoning off`. `KIRA_LLAMA_SERVER` selects the executable (default
  `llama-server`).
- Ready when `/health` answers 200; loading counts toward the job timeout.
- Each job posts to `/v1/chat/completions` (non-streaming) with the messages,
  sampling values (top-k 20 and the repetition penalty rules of
  `prompt.enhance`), `max_tokens`, `stop` and `cache_prompt: true`, so the
  already-processed conversation prefix is reused between turns.
- Restarted when the requested model, `contextLength`, `gpuLayers`,
  `kvCacheLocation` or `kvCacheType` differs from the loaded one, since these
  are launch arguments.
- A crash during a job fails that job with `chat-failed`; the next job
  relaunches the process.

**GPU coexistence.** The runner stops `llama-server` before any image job
(`image.generate`, `image.edit`, `image.upscale`) or `prompt.enhance` starts,
and after ten minutes without a chat job (configurable in
`ServiceConfiguration`). The next chat job reloads the model. The daemon
stops it on shutdown.

### `completion`: `llama-completion` per job

Reuses the `prompt.enhance` subprocess path. Messages are flattened into the
single user message: the system content first, then the transcript as
`User:` / `Assistant:` lines, ending with an instruction to write the next
assistant reply. The model reloads on every job. It receives the same
memory-placement arguments as the server engine. `stop` is applied by cutting
the output. This engine exists for comparison and as a fallback.

## Vertical Slices

### 1. Protocol and validation

**Behavior:** `text.chat` requests are validated and rejected or queued.

**Failing tests:** Every malformed field, role order, size limit and unknown key
fails before queueing; valid requests are accepted.

**Implementation:** Request parsing and validation in the daemon; capability
advertisement; `textChatEngines`.

### 2. `completion` engine

**Behavior:** A chat job runs through `llama-completion` and returns `text`.

**Failing tests:** A fake `llama-completion` receives the flattened prompt;
`stop` cutting, reasoning removal, timeout, output limit and disconnect
cancellation behave like `prompt.enhance`.

### 3. `server` engine

**Behavior:** A chat job runs through a managed `llama-server` and returns
`text`, `finishReason` and `usage`.

**Failing tests:** A fake `llama-server` (a small test HTTP executable) receives
the expected launch arguments (including each `gpuLayers` and KV-cache
variant) and request body; the process is reused across jobs with the same
launch settings, restarted when the model, context or memory placement
changes, stopped before an image job and after the idle period; a crash fails only the active job; a
context overflow maps to `chat-context-exceeded`.

### 4. SDK

**Behavior:** Clients submit chats and receive typed results.

**Failing tests:** `ServiceSdkTest` covers capability parsing, request
serialization, success and error results.

### 5. Documentation

Update `docs/protocol-v3.md`, `README.md`, `CONTRIBUTING.md` and `AGENTS.md`.

## Verification

```sh
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

Then, manually on the service host, run the same conversation with both
engines and several memory placements (for example `all` + `ram`, `all` +
`gpu` `q8_0`, `auto` + `gpu`). Compare reply quality, the first (cold) turn's
latency and later turns' latency.

The ten-minute job timeout applies to every turn, including a cold start that
loads the model.
