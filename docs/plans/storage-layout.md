# Split model and configuration storage

## Goal

Store model weights and service configuration in separate directory trees so
that configuration can be backed up frequently without copying large,
re-downloadable weights. The reference deployment uses a dedicated `/srv/lab`
volume shared with other lab services.

## Current layout

| Content | Location |
|---|---|
| Listener, port and token path | `/etc/nukleus/nukleus.conf` |
| LAN token | `/etc/nukleus/token` |
| Weights | `$XDG_DATA_HOME/nukleus/models/{,upscale,llm,companion}/` |
| Per-category `metadata.json` | beside the weights of each category |
| `model-routing.json` | at the model root |
| Jobs and llama-server key | `/run/nukleus` (`TMPDIR`) |

Catalogue configuration (display names, recommendations, shared VAE/LLM/MMPROJ
assignments, explicit-content classification, presets and editing qualities) is
interleaved with the weights.

## Target layout

```
/srv/lab/
├── models/                      # weights only, shared and not named after an app
│   ├── generation/
│   ├── upscale/
│   ├── llm/
│   └── companion/
└── config/nukleus/
    ├── service/                  # root:nukleus, read-only for the daemon
    │   ├── nukleus.conf
    │   └── token
    └── catalog/                  # written by the daemon through QSaveFile
        ├── generation.json
        ├── upscale.json
        ├── llm.json
        ├── companion.json
        └── model-routing.json
```

- Each `catalog/<category>.json` has the exact format of the former
  `metadata.json` of that category; filenames remain relative to the
  category's weight directory.
- `.part` staging files and per-model `companions/` subdirectories stay beside
  the weights, so hard-link publication remains on one filesystem.
- Publication order is unchanged: weights are published first, then the
  catalogue entry is written atomically.
- The weight tree may be shared with other applications; only the catalogue
  is specific to Nukleus. Every accepted file in a category directory
  is listed as a model, and Manager deletion currently removes the file.
  Before other applications use the tree, deletion of shared weights should
  remove only the catalogue entry (deployment slice).

## Slices

1. **Plan** (this document).
2. **Daemon paths.** `--models-dir DIR` and `--config-dir DIR` select the split
   layout; both are required together. The catalogue lives in
   `DIR/catalog`. Without them the daemon keeps the XDG layout and its legacy
   `kira-canvas` migration unchanged. The daemon creates the split directories
   at startup and fails early when they cannot be created.
3. **Migration command.** Copy a legacy model tree (for example a backup copy)
   into the split layout: weights to `<models-dir>/<category>/`, each
   `metadata.json` to `catalog/<category>.json`, `model-routing.json` to
   `catalog/`. Offer `--dry-run`, refuse non-empty destinations, never modify
   the source and never merge. Tests use disposable fixtures only.
4. **Deployment.** Installer options for both directories, preserved across
   updates; `nukleus.conf` provides `NUKLEUS_MODELS_DIR` and
   `NUKLEUS_CONFIG_DIR`. The systemd unit adds `RequiresMountsFor=` and
   restricts `ReadWritePaths=` to the weight tree, the catalogue directory and
   `/run/nukleus`, keeping `ProtectSystem=strict`, `ProtectHome` and
   `PrivateTmp`. `/etc/nukleus/nukleus.conf` remains the
   `EnvironmentFile` bootstrap.
5. **Documentation.** Update AGENTS.md, README and CONTRIBUTING.

## Backups

- `config/` is small and safe to back up while the service runs because every
  catalogue file is replaced atomically.
- `models/` can be backed up rarely or excluded. Restoring a catalogue without
  its weights must leave entries for missing files harmless.
