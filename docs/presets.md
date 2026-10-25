# Presets and routing

The service keeps a service-wide set of generation presets and editing qualities
that clients offer to their users. The Manager edits them through **Presets…**
(see [Manager](manager.md)); the model each one routes to must support the
operation (see [Models](models.md)).

## Presets and editing qualities

**Presets…** opens a service-wide tree with fixed Generation and Editing branches.
Generation starts with **Photorealism / Photoréalisme** and **Illustration**.
**Add preset** (+) below the tree creates an unassigned preset with default names;
**Remove preset** (×) removes the selected preset. Generation
keeps between 1 and 16 presets; drag/drop is not available.
Manager displays preset names in English regardless of its system locale.
Select a preset to edit its English and French names, choose its eligible model, edit **Steps** and **CFG Scale**
and its optional positive/negative prefixes, and choose how many **Identity
portraits** (1, 2 or 4, default 1) a client generates with it. **Editing** holds the
fixed **Fast**, **Balanced** and **Quality** qualities that clients offer for edits:
select one to choose its editing model, Steps/CFG and optional
positive/negative prefixes, and tick **Default quality** to make it the quality a client preselects
and uses for edits without a quality choice, such as applying an identity
(Balanced initially; exactly one quality is the default). Selecting a model
snapshots its recommended values (shared defaults when absent); Save persists
that pair, including manual reductions in steps. Catalogue refreshes never
replace saved values. Models can be reused across presets and editing.
Save persists the choices; Cancel discards them.
Presets have no prompt flow: LLM help (translation, prompt improvement) comes from
the **Default prompt translator** and **Default prompt enhancer** LLM checkboxes,
which clients run through `prompt.enhance`.
Mappings start unassigned and require explicit setup before a client can submit.
Save waits for acknowledgment; Cancel discards all draft edits. Assigned models
cannot be deleted or lose required capabilities until unassigned.

## Routing file and migrations

Version-6 `models/model-routing.json` stores both English and French labels,
each preset's and editing quality's model with Steps/CFG, the default editing
quality, and each preset's identity portrait count atomically; versions 1–4 load with one identity portrait, and
versions 4–5 copy their single editing model into all three qualities. Versions 1–3 stored
Fast/Balanced/Quality tiers: presets load with their **Fast** (`-low`) tier,
dropping the others, while editing keeps all three tiers and its permanent tier
(if any) becomes the default. Prompt flows saved by earlier versions are ignored.
Missing parameters are initialized from model recommendations, and the file is
rewritten (as version 6) only on explicit Save.
Version-1 custom entries are not migrated. Unreadable/malformed
configuration is never overwritten. Update the service before its clients.
Legacy per-model style protocol/metadata remain compatible with older clients,
but their management dialog is removed.
