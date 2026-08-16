# Contributing to Kira Service

Kira Service is a C++20 Qt 6 project. Build dependencies are CMake, Ninja, a C++20 compiler, and Qt 6.2+ Network, WebSockets, and Widgets development packages. `sd-cli` is only required on a machine that performs image work.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the daemon with `./build/src/kira-service`. Run the local administrator UI with `./build/src/kira-service-manager --gui`, or exercise the manager CLI with `./build/src/kira-service-manager --help`. Use the root installer only for Linux systemd deployments.

Sources are organized under `src/daemon/`, `src/manager/`, and `src/protocol/`.
The two executable targets remain declared by `src/CMakeLists.txt`, so their
build paths remain under `build/src/`.

To install only the built executables into a CMake prefix, run
`cmake --install build --prefix "$HOME/.local"`. Use
`scripts/install-kira-service.sh` when a complete Linux systemd installation
is required.

Keep protocol v3 compatible with existing Canvas clients. Do not create an inter-repository library; update the protocol document and each local implementation together.
