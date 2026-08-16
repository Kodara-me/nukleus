# Kira Service WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The service replies with `authenticated` and `capabilities`. `models.list` returns all `image-generation`, `upscaler`, and `llm-chat` categories. Each model contains only `id`, `name`, `fileName`, and `bytes`.

Model administrators send `models.install` with a client-generated `installId`, explicit category, direct Hugging Face resolve URL, and optional SHA-256. The service emits `models.install.accepted`, `models.install.progress`, `models.install.completed`, or an `error` containing that `installId`.

Image work uses `submit` with `image.generate` or `image.upscale`; each request must include a matching installed `modelId`. Upscale is fixed at 2×. Input and result image data are binary WebSocket messages; filesystem paths are never part of the protocol.
