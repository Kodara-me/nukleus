---
status: implemented
projects: [kira-service, kira-canvas]
---

# Per-model generation styles

## Status

Implemented across Service, SDK, Manager and Canvas. Deployment remains separate;
update the service before relying on styles in updated clients. Preserve
pre-existing working-copy changes in both repositories.

## Goal

Manage positive/negative prompt prefixes per generation model in Service Manager.
Return styles with the SDK model catalogue. Canvas selects a style but never
edits it; the service applies its prefixes.

## Scope

Include service persistence, protocol/SDK support, a Manager styles dialog and
Canvas's existing Style selector. Styles affect `image.generate`, including
identity portrait generation. Exclude editing/upscaling, style sharing,
import/export and automatic style selection.

## Existing Context

- `ModelCatalog` persists category-local `metadata.json` atomically.
- The daemon, SDK and Manager have separate model representations and existing
  catalogue-refresh paths.
- Canvas's `GenerationParametersWidget` already displays a disabled Style selector.
- Manager has a fixed-size window, shared KiraUi dialogs and acknowledged Save
  workflows.

## Vertical Slices

### 1. Service-owned styles returned with models

**Behavior:** Every generation model exposes its saved styles or starter presets.

**Components:** Protocol types, `ModelCatalog`, catalogue serialization, SDK and
Manager catalogue parsing.

**Input/output:** Existing model metadata → ordered `styles` array → typed model
styles.

**Invariant:** Reading a catalogue never writes metadata; an explicitly saved
empty list remains empty.

**Failing tests:** Catalogue exposes starter styles for legacy models; saved
custom/empty lists survive restart.

**Implementation:** Add the shared style value type, validation and JSON helpers;
extend metadata and model parsing/serialization.

**Verification/completion:** Service and SDK tests pass using the service
validation commands below; existing model installation, recommendations and
companions remain intact.

### 2. Manager edits styles for one model

**Behavior:** A **Styles…** button opens a dedicated editor for the selected
generation model.

**Components:** Manager dialog/client, new daemon operation and `ModelCatalog`
mutation.

**Input/output:** Edited style list → atomic metadata update → acknowledged result
and catalogue-change notification.

**Invariant:** Cancel performs no write; failures retain the draft; saving styles
preserves other metadata.

**Failing tests:** Save persists changes, Cancel discards them, invalid lists fail
without partial updates.

**Implementation:** Add list/form editing, Add/Remove and Save/Cancel; follow the
existing unsaved-model-changes guard before opening the dialog. The form edits
name, positive prefix and negative prefix in an in-memory draft. Prefix editors
support multiline text. Add appends a new style; Remove deletes the selected
draft entry. Save replaces the complete list in its displayed order. Disable
editing while saving, wait for acknowledgment before closing, and retain the
draft on failure. Use shared KiraUi components without changing the main window's
fixed geometry.

**Verification/completion:** Manager UI and service tests pass using the service
validation commands below; success appears only after service acknowledgment.

### 3. Selected styles affect generation

**Behavior:** Canvas sends a style ID; the service prefixes both prompts exactly
once.

**Components:** Generation request type, SDK submission and daemon request
handling.

**Input/output:** Model ID + optional style ID + original prompts → composed
prompts stored in the accepted job.

**Invariant:** The service resolves styles within the selected model and snapshots
composed prompts before uploads/queueing.

**Failing tests:** Fake backend receives the expected positive/negative strings;
later style edits do not alter an accepted job.

**Implementation:** Validate the style selection, compose prompts and reuse the
existing generation/conditioning lifecycle. Preserve existing nonblank user
prompt validation; validate conditioning restrictions against the composed
prompts before accepting the job.

**Verification/completion:** Service and SDK tests pass using the service
validation commands below. Requests without styles behave unchanged; unknown
style IDs fail explicitly.

### 4. Canvas selects server-provided styles

**Behavior:** The existing selector shows **None** followed by the selected
model's styles.

**Components:** Generation parameters widget, request assembly and catalogue
refresh.

**Input/output:** SDK model styles → selector and prefix tooltips → request
`styleId`.

**Invariant:** Prompt editors and their counters retain the user's original text.

**Failing tests:** Model switch/reset clears style selection; refresh preserves a
valid selection; generated requests contain the style ID and original prompts.

**Implementation:** Activate the existing selector, connect selection changes and
synchronize it with model changes. Disable it when the model has no styles,
retaining the None entry. Show both prefixes in each style's tooltip.

**Verification/completion:** Canvas UI/WebSocket tests pass using the Canvas
validation commands below with regular, identity portrait requests.

## Contracts and Interfaces

- Shared `PromptStyle`: `id`, `name`, `positivePrefix`, `negativePrefix`. Keep the
  value type and JSON/validation helpers in the framework-independent Protocol
  target so daemon, SDK and Manager use the same contract.
- Append `QList<PromptStyle> styles` to daemon, SDK and Manager model types,
  preserving existing aggregate initialization.
- Model catalogue JSON includes:

  ```json
  {
    "styles": [
      {
        "id": "cinematic",
        "name": "Cinematic",
        "positivePrefix": "cinematic lighting, dramatic composition, film still",
        "negativePrefix": ""
      }
    ]
  }
  ```

- Add `models.styles.configure` with opaque model `id` and complete replacement
  `styles` array. Return `models.styles.configure.completed` with the model ID
  and saved list. Reject missing models, non-generation categories and malformed
  lists through the existing model-operation error mechanism.
- Expose `configureModelStyles` and completion signaling in SDK/Manager clients;
  reuse existing model-operation error handling. Canvas does not call this API.
- After successful saving, broadcast `models.styles.changed` with the affected
  model ID to authenticated clients; connected clients request a fresh catalogue.
- Append optional `styleId` to `ImageGenerationRequest`; transmit it in generation
  options only when nonempty. Reject nonempty style selections on `submitEdit`.

## Domain Rules

- Styles belong only to image-generation models.
- IDs are stable and unique within their model. New custom styles receive UUIDs;
  renaming retains the ID. IDs are not editable in the Manager form.
- Require valid string fields, a nonblank ID/name and at least one nonblank prefix.
- Empty/absent `styleId` means None. Unknown IDs fail; never silently substitute
  another style.
- Compose each prompt by trimming and joining its nonempty prefix and user text
  with `, `. With None selected, preserve the existing prompt behavior unchanged.
- Preserve existing request validation and conditioning restrictions. Never
  insert `img` automatically.
- Apply styles only on the service, once. Store composed prompts in the accepted
  job so queue-time edits cannot change them.

## Infrastructure Design

Store `styles` beside existing model metadata fields using the existing atomic
read-modify-write path. Preserve recommendations, companions, classification,
model type and untouched entries. Add small shared value/validation helpers
rather than a new repository or persistence framework.

Canvas has no style storage or mutation controls. Old services that omit styles
yield **None** only; old clients that omit `styleId` retain normal generation
behavior. Keep protocol v3 additive and introduce no dependencies. Do not
synthesize starter styles in clients.

## Test Cases

Cover:

- Legacy metadata, restart persistence, explicit empty lists and preservation of
  unrelated metadata.
- Invalid categories, duplicate IDs, malformed fields and failed atomic saves.
- Manager add/edit/remove, Save acknowledgment, Cancel and retry after errors.
- SDK catalogue decoding, generation serialization and styles-change refresh.
- Positive-only/negative-only prefixes, empty negative prompts and no duplicate
  application.
- Unknown/deleted styles and accepted-job snapshots.
- Canvas None, switching models, refresh preservation/removal and New Image reset.
- Style selection alongside identity portraits without modifying their
  inputs or user prompts.
- Older catalogue responses without `styles` and generation requests without
  `styleId`.

Use the existing `ServiceTest`, `ServiceSdkTest`, `ManagerUiTest` and `CanvasTest`
fixtures. Never modify live installed models to exercise tests.

## Execution Order

Deliver slices 1–4 sequentially using red-green-refactor. Build/test after each
slice. Update both projects' README, contributor documentation, durable contracts
and protocol documentation; update applicable UI translations.

Deploy the service update before relying on styles in the updated clients. No
eager metadata migration or live-model edits during validation. Keep work
uncommitted unless the user requests a commit.

## Validation Commands

Run from the `kira-service` repository, using the existing configured dependency
prefixes:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

cmake -S ../kira-canvas -B ../kira-canvas/build -G Ninja
cmake --build ../kira-canvas/build
ctest --test-dir ../kira-canvas/build --output-on-failure

git diff --check
git -C ../kira-canvas diff --check
```

## Acceptance Criteria

Manager can edit styles independently for each generation model. The SDK returns
them with model listings. Canvas provides selection only. Backend execution
receives the composed prompts, while Canvas editors remain unchanged. Existing
generation remains unchanged with None selected.

## Assumptions and Risks

Confirmed choices: starter presets and a dedicated Manager dialog.

Models without a saved `styles` field expose these service-defined starters;
saving `[]` suppresses them:

| ID / name | Positive prefix | Negative prefix |
|---|---|---|
| `photographic` / Photographic | `photorealistic, natural lighting, fine detail` | `illustration, painting, cartoon` |
| `cinematic` / Cinematic | `cinematic lighting, dramatic composition, film still` | Empty |
| `illustration` / Illustration | `digital illustration, expressive colors, detailed artwork` | `photograph, photorealistic` |

These are editable starting points, not model-specific guarantees. Names display
as authored. None is always the initial selection; switching models or New Image
resets it. Refreshing the same model preserves the selected ID only while it
still exists.
