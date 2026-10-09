# Local analysis workflows

Repository-local skills for agents working on GameNative / SteamPSP. Commands run from the
repository root. Device, evidence and publication rules in `AGENTS.md` apply to every workflow.

| Workflow | Scope |
| --- | --- |
| [gamenative-stage-concurrency-analysis](gamenative-stage-concurrency-analysis/SKILL.md) | XR per-frame stage timelines across the Wine game process and the app process; six-frame concurrency view through the Archify viewer. Contract v1 is implemented but not yet captured on a device. |

Run a skill's tests with `python -m unittest discover -s skills/<skill>/tests`.
