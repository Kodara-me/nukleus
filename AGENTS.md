# Kira Service Development Contract

- Kira Service is a C++20 Qt 6 backend project. Keep the daemon and native manager separate executables; do not add a reusable library yet.
- The daemon owns model storage, Hugging Face downloads, temporary jobs, and one FIFO `sd-cli` worker. It never exposes service paths over WebSocket.
- Preserve protocol v3 unless an explicit compatibility slice changes it. Canvas and other clients select opaque model IDs and submit work; only the local manager exposes model installation UI.
- Use `$XDG_DATA_HOME/kira-service/models/` with `upscale/` and `llm/` subdirectories. Never overwrite installed model files. Keep the legacy-root migration safe and non-merging.
- Linux systemd deployment keeps the `kira-service` account, `/etc/kira-service`, `/var/lib/kira-service`, `/run/kira-service`, and fixed port `9384`.
- Build and run CTest after each slice. Keep README and CONTRIBUTING accurate.
