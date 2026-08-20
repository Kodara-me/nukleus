# Kira Service WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The version-3 authentication message is required for every client. Loopback peers may send an empty token. Non-loopback peers must send the configured shared LAN token.

The service replies with `authenticated` and `capabilities`. `models.list` returns all `image-generation`, `upscaler`, and `llm-chat` categories. Each model contains only `id`, `name`, `fileName`, and `bytes`.

Model administrators send `models.install` with a client-generated `installId`, explicit category, HTTPS model-file URL, and optional SHA-256. The service emits `models.install.accepted`, `models.install.progress`, `models.install.completed`, or an `error` containing that `installId`.

Model administrators may delete an installed model by its opaque ID:

```json
{"type":"models.delete","id":"upscaler:model.gguf"}
```

The service replies with `models.delete.completed` or an `error` containing the model `id`. Deletion is rejected while the model is referenced by active, queued, or pending-upload work. Filesystem paths are never accepted or returned.

Image work uses `submit` with `image.generate` or `image.upscale`; each request must include a matching installed `modelId`. Upscale is fixed at 2×. Input and result image data are binary WebSocket messages; filesystem paths are never part of the protocol.
