# Manager

`nukleusd` is the daemon. `nukleus-manager` is the administrator tool: a GUI
(`nukleus-manager --gui`) and local CLI operations. Models and their options are
described in [Models](models.md); presets are in [Presets](presets.md).

## GUI

`nukleus-manager --gui` opens the administrator UI with the shared dark Kira theme: a category/model tree on the
left and editable recommended options on the right. Its window content has a fixed,
non-resizable size of 1100 × 640 logical pixels, excluding the native title bar.
Saved geometry restores the window position; previous sizes and maximized states are ignored.
Select a model or its companion to edit recommendations, then Save to persist
them on the service. The shared add/edit form includes **Explicit content (NSFW)**,
unchecked by default; the service persists the flag and returns it with model listings.
Unsaved edits prompt before leaving the model.

Manager windows and dialogs use native platform title bars and buttons while
keeping the Kira content theme. Window movement/resizing is handled by the OS;
verify actual decorations on each target desktop in addition to offscreen tests.

### Connection

The GUI tries its last successful service connection at startup, defaulting to
`127.0.0.1:9384`. If connection or authentication fails, enter the server's IPv4
address (or `localhost`) and key in the connection dialog. Enter only the host;
the Manager builds `ws://<host>:9384`. A successful authentication saves the
connection for next launch; failed attempts and Cancel do not change saved settings.
Remote servers must have a reachable LAN listener and token configured; use a
trusted LAN or VPN. Browse imports remain local-only. CLI commands still use loopback.

### Model tree and options panel

Selecting any category or subtype leaves the right panel empty. Selecting a
model shows its required display name and relevant fields. Upscaler has no model
options, explicit-content checkbox or recommendations hint. LLM retains its
sampling/token fields but has no recommendations hint. The options each category
exposes are listed in [Models](models.md#recommended-options).

### Add model

Add model opens a landscape modal with HTTPS model/companion URLs on the left
and category-specific recommendations on the right, without scrolling.
Progress stays in that modal; success closes it and selects
the installed model. Closing during download cancels the installation and
cleans its temporary files. Only one installation is accepted across clients;
there is no download queue. The image execution queue is unchanged.
The manager connects to `127.0.0.1:9384` and retries while the service is offline.
The add and optional-file dialogs offer **Browse…** beside the model and companion
sources; see [Installation sources](models.md#installation-sources).

### Optional files

Companions can be removed independently through the Manager's **Optional files…**
dialog. Confirm **Remove** beside an attached role, then choose a new URL or
Browse source and apply the replacement. Removal preserves the primary, other
companions, recommendations and explicit-content classification; queued/active
models and models receiving files cannot lose companions. The matching protocol
operation is described in [Models](models.md#shared-companions).

### Animation models

In the Manager, the Animation branch lists each model with its attached files
and its VAE and text encoder, and marks every missing one. The add form takes
the diffusion model, the optional high-noise model and LoRAs (URL or
**Browse…**), and for the VAE and the text encoder either an installed companion
of the right subtype or **New from URL or file…**. On **Start download**, the
Manager installs each new companion first (named after its file), then the model
with their IDs, one installation at a time. Closing the form cancels the running
installation; companions already installed stay in the catalogue and can be
selected again. **Optional files…** attaches or removes the high-noise model and
the LoRAs of an installed model, and attaches again a file reported missing.

The options panel of an animation model also edits its default video options,
grouped as Video, Sampling, LoRA, Cache, Memory and VAE tiling, with each
high-noise option beside its low-noise counterpart. Every field may be left
empty: an empty field means no model default, so a job falls back to sd-cli's
own default for it. Save writes the filled fields only, with the VAE and text
encoder IDs; a value outside its range shows a message and blocks Save.

## CLI

The manager also provides local CLI operations:

```sh
nukleus-manager list
nukleus-manager add https://huggingface.co/owner/repo/resolve/main/model.gguf --category upscaler --display-name "Upscaler"
nukleus-manager add https://example.com/projector.gguf --category companion --companion-type mmproj --display-name "Vision projector"
nukleus-manager add https://example.com/model.gguf --category llm-chat --display-name "Chat" --mmproj-model companion:projector.gguf
nukleus-manager add https://example.com/edit.safetensors --category image-generation --display-name "Image model" --vae https://example.com/vae.safetensors
nukleus-manager attach image-generation:existing.gguf --model-type diffusion --clip-l https://example.com/clip_l.safetensors --clip-g https://example.com/clip_g.safetensors --vae https://example.com/vae.safetensors
nukleus-manager delete upscaler:model.gguf
```

Use `--help` for all options. CLI and GUI manager connections are local-only and do not require manual host or token configuration.
The animation commands are in [Models](models.md#animation-models).
