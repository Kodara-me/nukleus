# Kira Service Development Contract

- Kira Service is a C++20 Qt 6 backend project. Keep the daemon and native manager separate executables, and keep reusable image transport code in the installable SDK under `src/sdk/`.
- The daemon owns model storage, HTTPS model downloads, temporary jobs, and a FIFO execution queue for `sd-cli` image jobs and one-shot `llama-cli` prompt conversion. It never exposes service paths over WebSocket.
- The manager is local-only: it connects to `127.0.0.1:9384`, supports GUI and `list`/`add`/`delete` CLI modes, and is installed into the invoking user's `~/.local/bin`.
- Sources are organized under `src/daemon/`, `src/manager/`, and `src/sdk/`; the daemon and manager executable targets remain declared by `src/CMakeLists.txt`. Export `KiraService::Protocol` and `KiraService::SDK` from the SDK.
- The daemon always listens on `127.0.0.1`; a configured LAN IPv4 address adds a second listener on the same port. Loopback authentication is tokenless; LAN authentication remains token-protected.
- Preserve protocol v3 unless an explicit compatibility slice changes it. Canvas and other clients select opaque model IDs and submit work; Canvas may expose installation only for explicitly curated packages through the SDK, while the local manager remains the general model-management UI.
- CMake installs the daemon to `libexec/kira-service/` and the manager to `bin/` under the selected prefix; the shell installer remains authoritative for systemd deployment and configuration.
- CMake also installs SDK headers and static libraries and generates the `KiraServiceSdk` package. The SDK supports image operations and one-shot LLM prompt conversion; general LLM submission remains future work.
- Use `$XDG_DATA_HOME/kira-service/models/` with `upscale/` and `llm/` subdirectories. Never overwrite installed model files. Keep the legacy-root migration safe and non-merging.
- Linux systemd deployment keeps the `kira-service` account, `/etc/kira-service`, `/var/lib/kira-service`, `/run/kira-service`, and fixed port `9384`.
- Build and run CTest after each slice. Keep README and CONTRIBUTING accurate.
