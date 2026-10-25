# Identity portrait packages

A client can generate portraits through ordinary image generation and package
the selected PNGs and face/hair prompts locally. The SDK IdentityPackage helper writes
a compressed .kiraid ZIP containing manifest.json (type kira-identity, version 2),
reference.png and up to three additional ordered reference PNGs, plus seed metadata
and generation/edit prompts describing how to use the references.
No extraction backend or binary payload is required. Existing user models and
older identity files are retained on disk.

The number of portraits a preset generates is set in [Presets](presets.md#presets-and-editing-qualities).
