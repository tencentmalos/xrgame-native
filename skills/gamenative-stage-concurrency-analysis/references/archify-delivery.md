# GameNative domain renderer on the Archify viewer

Vendored from Azahar (see provenance.md). This is a domain adapter to the installed tencentmalos
Archify fork; Azahar audited it at
`1df62406f8abc8b303cd23783f6dd713d900937a`. It uses `applyTemplate`, the shared template and
`paletteRegistry`, not a public plugin API. API/slot changes must fail explicitly and be reviewed on
upstream sync. The receipt hashes these runtime files and all three adapter scripts.

## Why the stage model stays domain-native

The stage bundle owns six game-frame bands (fid), the fid token contract, the cross-process clock
alignment, inline wait phases, XR loop pins and frame annotations.
The generic Archify timeline has a different parent/lane model and counts active lanes instead of
active stages. Mapping all these into generic work/detail/wait would lose meaning or double count.
The bridge therefore keeps the existing deterministic domain SVG and wraps it in the shared viewer.
It applies semantic color tokens only to presentation, without changing relation or timestamp payloads.

A mixed-clock domain chart retains its alignment diagnostic. An uncalibrated GPU envelope is context,
not an overlap measurement; do not relabel it calibrated to pass a generic timeline validator.

## Commands and artifacts

`deliver_stage_diagram.py --input bundle.json --output-dir <new-directory> --evidence-kind measured`
selects six complete frames by default. Use `--select-frame-start <exact-token>` for another start,
`--select-frame-count` only for an explicitly different review window. `--palette` selects studio/ocean/
sunset/feishu; `--theme` selects the initial HTML and standalone SVG mode. Locale defaults to zh-CN.

Resolve Archify with `--archify`, then `ARCHIFY_ROOT`, then installed Codex/Claude personal skill paths,
then the copy bundled with the litep MCP (`~/PicoMcp/litep/versions/*/skills/archify`).
No dependency download or fallback to an old palette happens during delivery. The same repo-local
command works from both assistants; no separate copy of the domain implementation is installed globally.

A private sibling directory holds all outputs until strict analysis and bridge rendering pass:

- `stage.bundle.json`: byte-identical original input; `stage.selected.json`: normalized selected data,
  including original unclipped interval endpoints where normalization knows them.
- `stage.report.json` and `stage.review.md`: original domain statistics/diagnostics, not lane metrics.
- `stage.html`: shared Archify viewer with the complete domain SVG and its hover/pin behavior.
- `stage.svg`: same geometry and data with resolved explicit hex presentation colors; fixed authored theme.
- `archify-palettes.json`: registry snapshot from the chosen installed package.
- `delivery.receipt.json`: source/artifact/implementation SHA-256, frame/interval count, palette,
  theme, diagnostics, separate pending browser/perceptual state.

Only then is the directory renamed into place. Existing directories are refused and retained;
failed render/analysis leaves no published partial run. A passed domain run is not an Archify
showcase pass, a profiler capture identity audit, or visual approval. Preserve the browser
receipt beside it and write a separate perceptual record after viewing screenshots; do not rewrite
the delivery hash.

## Presentation contract

Stage/role colors consume the shared Archify registry. Fixed legacy interval colors are presentation
hints and are replaced with role/stage category tokens in this themed view; the input snapshot preserves
them. Unknown source roles keep their label. Inline phases and role legends retain their labels.
All drawing coordinates and semantic attributes survive wrapping, including exact token strings.
Subpixel intervals are not widened to fit labels. Long/source detail remains in hover/pin metadata.

Standalone SVG resolves CSS variables before rasterization, because document rasterizers may not support them. HTML keeps live tokens for palette/theme switching.

The SVG script locates its own SVG root rather than `document.documentElement`, so coordinate transforms
work when embedded in HTML. Domain CSS selectors are scoped to that root to avoid overriding viewer UI.
The viewer handles export; static SVG and sanitized previews do not imply interactive document support.
Use `render_stage_preview.cjs` for document PNGs. Confirm all four palettes in both themes, actual
interval selection, and exported SVG geometry when changing the adapter.
