# Contributing to Kira Service

Kira Service is a C++20 Qt 6 project. Build dependencies are CMake, Ninja, a C++20 compiler, and Qt 6.2+ Network, WebSockets, and Widgets development packages. `sd-cli` is required on a machine that performs image work and `llama-cli` is required for prompt conversion.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the daemon with `./build/src/kira-service`. Run the local administrator UI with `./build/src/kira-service-manager --gui`, or exercise the manager CLI with `./build/src/kira-service-manager --help`. Use the root installer only for Linux systemd deployments.

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
and optional paired companion metadata. The SDK supports one-shot prompt
conversion for installed LLM models, but not general LLM chat.
