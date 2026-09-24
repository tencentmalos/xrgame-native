@AGENTS.md

`AGENTS.md` holds the shared facts and constraints; this file is only the Claude entry point.

- Default to Chinese when talking with the user.
- Start from [docs/specs/xrgame-native-v1.md](docs/specs/xrgame-native-v1.md). Work packages run in the order WP0 → WP7, with the dependencies and exit criteria given there.
  - WP1 (Steam game installation) comes first because it supplies the test games for everything after it.
  - Do not split work packages into micro-specs.
  - Do not skip the measurement steps that later decisions depend on: the WP4 behavior matrix, the WP5.1 capability probe, and the WP7 baseline.
- Before changing anything under `references/`, read that checkout's own instructions (for example FEX's AI-contribution ban).
