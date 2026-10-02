# Kira Service WebSocket protocol v3

Clients authenticate first:

```json
{"type":"authenticate","version":3,"token":"..."}
```

The version-3 authentication message is required for every client. Loopback peers may send an empty token. Non-loopback peers must send the configured shared LAN token.

The service replies with `authenticated` and `capabilities`. `models.list` returns the `image-generation`, `edition`, `upscaler`, `llm-chat`, and `ip-adapter` categories. Each model contains `id`, `name`, `fileName`, and `bytes`; a model with a companion also includes the category-relative `companionFileName` and `companionRole` (`lora`, `mmproj`, or `vae`).

Model administrators send `models.install` with a client-generated `installId`, explicit category, and HTTPS model-file URL. Optional `companionUrl` is a LoRA for `ip-adapter`, mmproj for `llm-chat`, or VAE for `image-generation` and `edition`; it is rejected for `upscaler`. mmproj requires a `.gguf` filename; VAE and LoRA accept `.safetensors`, `.ckpt`, or `.gguf`. SHA-256 is no longer checked or accepted by the SDK/manager; legacy protocol `sha256` fields are ignored. The service stages both downloads and commits the primary, companion, and relationship metadata together, so a failed or canceled pair leaves no usable catalog entry. A restart preserves the relationship; deletion removes both files. Listings and install completion expose the companion filename and role, never absolute paths. This does not change job operations or `sd-cli` invocation. The service emits `models.install.accepted`, `models.install.progress`, `models.install.completed`, or an `error` containing that `installId`.

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
