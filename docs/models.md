# Models

What the service stores as models, how they are installed and how their
recommended options work. The administrator tools that manage them are in
[Manager](manager.md); the jobs that use them are in [Jobs](jobs.md).

## Categories

Supported categories are Image generation, Upscaler, LLM, Companion and Animation.

Image-generation models support generation and image.edit with uploaded reference images.

Editing reuses **Image generation** models directly. image.edit accepts their
original opaque IDs and resolves the same primary weights, model type and companions.
There is no separate editing category. **Can Edit** and **Can Generate** control
which operations each model supports; at least one must be checked. Only models that
can edit use reference images, also when generating: the service rejects references
sent to any other model. Select an
eligible model as the default edit model a client offers. Missing flags preserve both
operations for existing models. Existing legacy model directories remain on disk.

Model installation preserves existing files: filename collisions receive a
numeric suffix before the extension. Manager's add and configuration forms
require a nonblank display name. Model labels use that name; opaque IDs remain unchanged when renaming a label.
CLI add requires `--display-name "My model"`; an explicit empty rename is rejected.

## Recommended options

Recommended options are metadata returned to clients, not enforced execution
settings. Image generation expose square resolution (256–2048 in steps
of 256), steps, CFG scale, sampler, scheduler, generation/edit capabilities and the prompt
format clients write for it (natural sentences, short phrases by default, or tags). LLM exposes
temperature, top-p, context length, and maximum tokens; Upscaler has no options or explicit-content control (execution remains fixed at 2×); Existing
models without recommendations remain usable; proposed defaults are saved only
on an explicit Save.

Image models save `canEdit`, `canGenerate`, `vaeModelId` and `textEncoderModelId`
inside recommendedOptions; LLM saves `canBeTextEncoder` (default false) there.
LLM forms also offer **Default prompt enhancer**, **Default prompt translator** and
**Default image describer** (`defaultPromptEnhancer`/`defaultPromptTranslator`/
`defaultImageDescriber`): at most one LLM holds each role, possibly the same one,
and saving a checked role moves it to that model. An LLM with an **MMPROJ
companion** can see images: text chats may attach up to eight PNG or JPEG images.
The service validates references and capabilities before accepting jobs. Legacy
attachment APIs/CLI remain compatible and existing files remain on disk; legacy
VAE/LLM files are fallback weights when no shared assignment is selected. Legacy
CLIP and image-level mmproj attachments are no longer passed to sd-cli.
**Optional files…** retains the legacy VAE removal/replacement workflow.
Editing jobs use image.edit with uploaded reference-image bytes.

## Image models

Image models can be complete checkpoints or standalone diffusion models (for
example, a split UNet GGUF). The add dialog includes **Model contents** plus
dropdowns for an installed **Companion / VAE** and an installed **LLM** marked
**Can be used as text encoder**, both with None. These are shared references;
deleting the image model never deletes their weights. Assigned models cannot be
deleted, retyped or have text encoder eligibility removed until detached.
CLIP-L, CLIP-G, Clip Skip and the image model's optional mmproj controls are removed.
The service passes assignments as `--vae` and `--llm`; the selected LLM's MMPROJ
assignment supplies `--llm_vision` when present. Standalone models use `--diffusion-model`; complete checkpoints use
`--model`. See the [upstream CLI options](https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/common/common.cpp).

## Shared companions

Companion entries are grouped as VAE, MMPROJ (vision projector) and T5-XXL
(animation text encoder) in Manager.
Install them with their own required names. VAE supports safetensors, ckpt and
GGUF; MMPROJ supports GGUF. LLM forms replace the optional mmproj URL with a
dropdown of installed MMPROJ entries, plus None. Save persists the selected ID;
clearing it detaches the relationship without deleting the companion.
Assigned companions cannot be deleted or retyped until detached from every LLM.
The current prompt-enhancement operation remains text-only.

Companions can be removed independently through the Manager's **Optional files…**
dialog (see [Manager](manager.md#optional-files)).

Protocol v3 adds `models.companion.delete` with opaque model `id` and companion
`role`, returning `models.companion.delete.completed` with both fields or an error.
SDK/Manager clients expose `deleteCompanion(id, role)` and `companionDeleted`.
Replacement uses the existing attachment install operation; it is a separate step,
so a failed replacement leaves the removed role absent. Legacy single-companion
metadata is supported and updated without discarding unrelated metadata.

## Animation models

The Animation category (`animation`) keeps image-to-video models such as Wan 2.2
I2V with every file they need. Its primary file is the low-noise diffusion
model; the high-noise diffusion model and a speed-up LoRA for each stage are
optional attachments (`high-noise`, `lora-low-noise`, `lora-high-noise`), and the
VAE and the T5-XXL text encoder are shared companions selected by ID
(`vaeModelId`, `textEncoderModelId`, both required). One `models.install`
downloads the model and its attachments; optional files can be attached or
removed later without downloading the model again. `models.list` reports the
roles whose file or companion is absent in `missingFiles`; see
[the protocol](protocol-v3.md#animation-models). The Manager's Animation branch
is described in [Manager](manager.md#animation-models). To run an animation
model see [Animating an image](jobs.md#animating-an-image).

```sh
nukleus-manager add https://example.com/umt5-xxl-encoder-Q8_0.gguf --category companion --companion-type t5xxl --display-name "UMT5-XXL"
nukleus-manager add https://example.com/wan2.2_i2v_low_noise_14B_Q4_K_M.gguf --category animation --display-name "Wan 2.2 I2V" \
    --vae-model companion:wan_2.1_vae.safetensors --text-encoder-model companion:umt5-xxl-encoder-Q8_0.gguf \
    --high-noise https://example.com/wan2.2_i2v_high_noise_14B_Q4_K_M.gguf \
    --lora-low-noise https://example.com/low_noise_lightx2v_4step.safetensors \
    --lora-high-noise https://example.com/high_noise_lightx2v_4step.safetensors
nukleus-manager attach animation:wan2.2_i2v_low_noise_14B_Q4_K_M.gguf --lora-high-noise https://example.com/high_noise_lightx2v_4step.safetensors
nukleus-manager attach animation:wan2.2_i2v_low_noise_14B_Q4_K_M.gguf --vae-model companion:other_vae.safetensors
```

`--vae-model` and `--text-encoder-model` are required for `add`; on `attach`,
either one replaces that companion and keeps the other.

Removed model directories and existing identity files are retained on disk.
Rebuild SDK consumers and deploy the updated Service alongside the updated clients.

## Installation sources

Manager add/optional-file dialogs offer **Browse…** beside the model and companion
sources. Selected local files are copied by the service through the same staged,
atomic installation workflow as downloads; originals are retained and existing
installed files are never overwritten. Installation requests may use `file://`
URLs only from loopback clients. LAN clients remain restricted to HTTPS sources.
The Manager streams selected files from a temporary localhost HTTP source with
an unguessable per-file URL. The service copies these through its existing download
pipeline, so systemd ProtectHome and PrivateTmp remain enabled. Loopback installation
clients may supply HTTP sources on 127.0.0.1; LAN requests cannot. Model metadata stores managed relative filenames,
never the source paths. No Hugging Face token or login is stored or required for
local imports.

## Storage layout

Service models live in `$XDG_DATA_HOME/nukleus/models/`: image-generation files in `models/`, upscalers in `models/upscale/`, LLM files in `models/llm/`, animation (image-to-video) models in `models/animation/`, and shared VAE/MMPROJ/T5-XXL companions in `models/companion/`. Assignments use opaque IDs and never expose service paths or delete shared files with a primary. The manager accepts HTTPS sources and local imports, never overwriting installed weights. VAE accepts `.safetensors`, `.ckpt`, or `.gguf`; MMPROJ requires `.gguf`; T5-XXL accepts `.gguf` or `.safetensors`. Hugging Face `blob` links normalize to `resolve` URLs. SHA-256 verification is not supported. Legacy category-local attachments remain stored under `companions/<primary filename>/`; legacy VAE/LLM weights remain usable as fallbacks. modelType defaults to checkpoint when absent.

`nukleusd --models-dir DIR --config-dir DIR` selects a split layout instead: weights in `models-dir/{generation,upscale,llm,companion,animation}/` and the catalogue (`generation.json`, `upscale.json`, `llm.json`, `companion.json`, `animation.json`, each in the former category `metadata.json` format, plus `model-routing.json`) in `config-dir/catalog/`. Both options are required together; the daemon creates the directories at startup. See `plans/storage-layout.md`.

The Linux installer moves legacy models from `$XDG_DATA_HOME/kira-canvas/models/` during an upgrade. It aborts without changing either directory if both model roots already exist. See [Deployment](deployment.md).
