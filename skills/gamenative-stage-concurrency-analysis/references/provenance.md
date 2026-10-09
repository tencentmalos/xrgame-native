# Provenance

Vendored on 2026-10-09 from the Azahar workspace (`C:\workspace\emulations\3ds\azahar`, branch
`codex/turnip-mesh-prototype` @ `1d7e396a3`), directory `skills/azahar-stage-concurrency-analysis`,
last changed in `87789aaad`. Azahar is GPL-2.0-or-later; this repository is GPL-3.0, which may
include it.

| File | Change |
| --- | --- |
| `scripts/analyze_stage_intervals.py` | docstring, tool name, SVG id `gamenative-stage-diagram`, SVG description; the review omits the Azahar draw-workload table when a bundle has no draw data. Interval math unchanged. |
| `scripts/deliver_stage_diagram.py` | docstring, contract strings, Archify search also covers the litep MCP bundle. |
| `scripts/wrap_archify_stage.mjs` | SVG id and `data-domain="gamenative-stage-v2"`. |
| `scripts/render_stage_preview.cjs` | unchanged. |
| `tests/test_analyze_stage_intervals.py`, `tests/fixtures/synthetic-six-frame.stage.json` | unchanged. |
| `tests/test_deliver_stage_diagram.py` | dropped the test that depends on Azahar's XR builder. |
| `tests/test_archify_stage_browser.mjs` | environment variable `GAMENATIVE_STAGE_TEST_BUNDLE`, SVG id. |
| `references/stage-bundle.schema.json` | title; `stageOptions` documents `layout`, `summary` and `lane`, which the analyzer already accepts. |
| `references/archify-delivery.md` | adapted wording. |

Not vendored: Azahar's XR builder and its PICA/Guest token rules, the Guest/Host completion map
renderer, and the Feishu review workflow. GameNative's builder is `scripts/build_gn_xr_stage_bundle.py`.

Known upstream drift when vendored: Azahar's XR builder still looks for
`Vulkan Guest GPU Completion Wait` (renamed in `b2f7346ef`), the `OpenXR Coordinator Wait *` scopes
(removed in `5c4479867`) and `PICA Host Vulkan Replay Draws` (no longer published after `c29d6cc4a`).
None of that code was copied.

To resync the engine, diff the Azahar directory against these files, re-apply the renames above and
run `python -m unittest discover -s tests`.
