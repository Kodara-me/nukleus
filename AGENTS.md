# Kira Service Development Contract

- Kira Service is a C++20 Qt 6 backend project. Keep the daemon and native manager separate executables, and keep reusable image transport code in the installable SDK under `src/sdk/`.
- The daemon owns model storage, a single active HTTPS model installation, recommended model options, temporary jobs, and a FIFO execution queue for `sd-cli` image jobs. It never exposes service paths over WebSocket.
- The manager is local-only: it connects to `127.0.0.1:9384`, supports GUI and `list`/`add`/`delete` CLI modes, and is installed into the invoking user's `~/.local/bin`.
- Sources are organized under `src/daemon/`, `src/manager/`, and `src/sdk/`; the daemon and manager executable targets remain declared by `src/CMakeLists.txt`. Export `KiraService::Protocol` and `KiraService::SDK` from the SDK.
- The daemon always listens on `127.0.0.1`; a configured LAN IPv4 address adds a second listener on the same port. Loopback authentication is tokenless; LAN authentication remains token-protected.
- Preserve protocol v3 unless an explicit compatibility slice changes it. Canvas and other clients select opaque model IDs and submit work; Canvas may expose installation only for explicitly curated packages through the SDK, while the local manager remains the general model-management UI.
- CMake installs the daemon to `libexec/kira-service/` and the manager to `bin/` under the selected prefix; the shell installer remains authoritative for systemd deployment and configuration.
- CMake also installs SDK headers and static libraries and generates the `KiraServiceSdk` package. The SDK supports image operations.
- Use `$XDG_DATA_HOME/kira-service/models/` with `upscale/`, `llm/`, and `edition/` subdirectories. The `edition` category manages image-editing model files; editing jobs remain outside its current scope. Never overwrite installed model files. Keep the legacy-root migration safe and non-merging.
- Linux systemd deployment keeps the `kira-service` account, `/etc/kira-service`, `/var/lib/kira-service`, `/run/kira-service`, and fixed port `9384`.
- Optional companions use category-local persistent metadata: mmproj for LLM-chat, VAE for Image-generation/Edition, and LoRA for IP-Adapter. Install and delete them with their primary model; expose only category-relative companion filenames and roles. Do not perform SHA-256 download checks. Image-generation/Edition also support role-specific optional CLIP-L, CLIP-G and VAE files through companionUrls/companionFiles, with modelType checkpoint (default) or diffusion. Existing image models accept companion attachment via models.install with an opaque id; never redownload or overwrite the primary. Stage all files and publish metadata atomically; cancellation/failure must preserve an existing primary and recommendations. Keep legacy companionUrl, companionFileName and string metadata compatible. Image-generation companions are resolved internally and passed with --clip_l, --clip_g and --vae; diffusion models use --diffusion-model and checkpoints use --model; mmproj and LoRA remain catalog attachments. Validate generation values/types before queuing, omit automatic sampler/scheduler flags, and preserve numeric precision. Never auto-apply recommendedOptions to jobs.
- Build and run CTest after each slice. Keep README and CONTRIBUTING accurate.

## Manager UI and recommended options

- Link only the manager/UI tests to KiraUi::Widgets 0.2. Default to the sibling
  checkout, allow KIRA_UI_SOURCE_DIR, and fall back to an installed package.
  Keep daemon and SDK independent of the visual framework.
- Use native OS title bars and window/dialog buttons; keep the Kira content theme.
- Apply KiraUi::Theme only in GUI mode. Use the shared MainWindow/Dialog, panels,
  tree and controls. Preserve the local CLI and fixed loopback connection.
- Use a landscape model tree/details splitter, default 1300×680 and minimum
  1000×550. Persist MainWindow/Geometry and MainWindow/ModelsSplitter with Qt.
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
  size; IP-Adapter uses strength. Hide unrelated fields.
- Preserve legacy metadata.json companion strings and untouched entries when
  adding object entries. Write atomically with QSaveFile; reject mutation on
  unreadable metadata. Keep existing models without recommendations usable.
- Embed the manager PNG icon exports at startup in GUI mode. Keep generated master
  artwork and provenance under assets/icons; do not add runtime asset conversion.
- Keep documentation in English. UI tests use isolated settings and disposable
  local service fixtures; never mutate live installed models to exercise tests.
