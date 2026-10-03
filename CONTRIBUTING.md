# Contributing to Kira Service

Kira Service is a C++20 Qt 6 project. Build dependencies are CMake, Ninja, a C++20 compiler, and Qt 6.2+ Network, WebSockets, and Widgets, SVG, and LinguistTools development packages, plus Kira UI Framework 0.2. `sd-cli` is required on a machine that performs image work.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the daemon with `./build/src/kira-service`. Run the local administrator UI with `./build/src/kira-service-manager --gui`, or exercise the manager CLI with `./build/src/kira-service-manager --help`. Use the root installer only for Linux systemd deployments.

Image-editing models use the `edition` catalog category and `models/edition/` storage; the manager GUI, CLI, and SDK support their installation, listing, and deletion. This category does not add an editing job operation.

Sources are organized under `src/daemon/`, `src/manager/`, and `src/sdk/`.
The two executable targets remain declared by `src/CMakeLists.txt`, so their
build paths remain under `build/src/`.

To install only the built executables into a CMake prefix, run
`cmake --install build --prefix "$HOME/.local"`. Use
`scripts/install-kira-service.sh` when a complete Linux systemd installation
is required.

The SDK targets are `KiraService::Protocol` and `KiraService::SDK`. A source
checkout can add `src/sdk` directly, while installed consumers use
`find_package(KiraServiceSdk CONFIG REQUIRED)`. The SDK owns WebSocket and
protocol behavior; applications own settings, files, and UI messages. Keep
protocol v3 compatible. Model management includes the `ip-adapter` category
and optional paired companion metadata (IP-Adapter LoRA, LLM mmproj, or Image-generation/Edition VAE).
The SDK install method accepts an optional companion URL instead of a SHA-256
checksum. Download checksums are no longer supported; runtime invocation is unchanged.

## Shared manager UI

CMake defaults `KIRA_UI_SOURCE_DIR` to `../kira-ui-framework`. Override it with
`-DKIRA_UI_SOURCE_DIR=/path/to/kira-ui-framework`. Without a source checkout,
provide the installed `KiraUi` package through `CMAKE_PREFIX_PATH`. Only the
manager and its UI tests link `KiraUi::Widgets`; embedded SDK consumers do not
acquire GUI dependencies. Theme and window icons are initialized only in GUI
mode. Original icon artwork and prebuilt PNG sizes are committed under
`assets/icons/`; no image conversion tools are required to build or run.

Manager options use one category-aware form in the details panel and download
modal. Shared defaults and strict wire validation live in the Protocol target's
`ModelOptions` API. Install clients may supply optional `recommendedOptions`;
`configureModel()` replaces a model's complete recommendation object, and
`cancelInstall()` cancels an owned active installation. These APIs do not alter
image job arguments or enable new execution categories.

Category-local `metadata.json` accepts legacy companion strings and new objects
containing `companionFileName` and/or `recommendedOptions`. Writes preserve
untouched entries and use QSaveFile. Invalid/unreadable JSON blocks mutation
instead of overwriting metadata. Do not edit users' installed files for tests.

The tests create disposable model stores and local HTTP/WebSocket servers.
Run them with local networking permitted. The unavailable-manager CLI check is
skipped if a real service already responds on port 9384. UI tests isolate
QSettings and use the offscreen platform. For visual review:

```sh
mkdir -p /tmp/kira-manager-review
QT_QPA_PLATFORM=offscreen KIRA_MANAGER_SCREENSHOT_DIR=/tmp/kira-manager-review ./build/tests/kira-service-manager-ui-test
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 KIRA_MANAGER_SCREENSHOT_DIR=/tmp/kira-manager-review ./build/tests/kira-service-manager-ui-test
```

Validate both 1300 × 680 and 1000 × 550 and review the add-model modal. Qt-only
manager UI code follows the framework's cross-platform behavior; the existing
service deployment, CLI and model-file commit operations retain their platform
requirements. Check available platforms and report any untested ones.

Manager windows and dialogs use native platform title bars and buttons while
keeping the Kira content theme. Window movement/resizing is handled by the OS;
verify actual decorations on each target desktop in addition to offscreen tests.

Generation option validation and backend arguments are covered by a disposable
POSIX fake sd-cli fixture: no installed model or GPU is used. Tests cover automatic
and explicit sampler/scheduler, CFG precision, malformed/out-of-range options,
optional defaults, VAE arguments for direct and uploaded-input jobs, and result
bytes. Rebuild and rerun the Linux installer to update a systemd-installed daemon;
a workspace build alone does not update the running service.
