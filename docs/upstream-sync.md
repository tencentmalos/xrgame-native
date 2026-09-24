# Upstream sync log

`malos/main` periodically merges `upstream/master` (utkarshdalal/GameNative). This file records each sync: the upstream range, the conflicts and how they were resolved, and the build checks run afterwards. Spec reference: WP0 in [specs/xrgame-native-v1.md](specs/xrgame-native-v1.md).

## Procedure

```bash
git fetch upstream
git switch -c sync/upstream-<yyyymmdd> malos/main
git merge --no-ff upstream/master
# resolve conflicts; keep our changes in the picoXr source set and build switches
./gradlew :app:assemblePicoXrDebug :app:assembleModernDebug
```

Then merge the sync branch into `malos/main` (the user confirms each commit and push) and add an entry below.

## Conflict hotspots

These are the upstream-owned files that our changes touch. Churn is the number of upstream commits touching each file, measured with `git log --since=2026-06-26 --until=2026-09-24 upstream/master -- <file>` on 2026-09-24. Upstream made 308 commits in that 90-day window.

| File | Our change | Upstream commits, last 90 d | Upstream commits, all time |
|---|---|---|---|
| `app/build.gradle.kts` | WP0: `picoXr` flavor, source set, `xrgame` signing config, `androidComponents` block | 23 | 182 |
| `ubuntufs/build.gradle.kts` | WP0: `picoXr` flavor and variant filter | 0 | 6 |
| `.gitmodules` | Setup: `references/*` entries appended at the end | 0 | 2 |
| `gradle/libs.versions.toml` | Expected in WP1 (JavaSteam source build) | 6 | 78 |
| `settings.gradle.kts` | Expected in WP1 (drop the Sonatype snapshots repository) | 0 | 8 |
| `app/src/main/AndroidManifest.xml` | None so far; picoXr manifest entries go in `src/picoXr/AndroidManifest.xml` | 10 | 47 |
| `app/src/main/java/app/gamenative/service/SteamService.kt` | None so far. If WP1 has to touch it, prefer a small hook over inline edits | 27 | 221 |
| `app/src/main/java/app/gamenative/PrefManager.kt` | None so far. Same rule as `SteamService.kt` | 30 | 114 |

Our new files (`src/picoXr/**`, `.github/workflows/xrgame-picoxr.yml`, `docs/**`, `references/**`, `AGENTS.md`, `CLAUDE.md`) do not exist upstream and cannot conflict.

## Log

### 2026-09-24: first drill (no upstream delta)

- `git fetch upstream` at 2026-09-24: `upstream/master` = `ebde76e9`, the fork base. `git merge-base --is-ancestor upstream/master malos/main` succeeds, and `git log ebde76e9..upstream/master` is empty.
- A merge would be a no-op ("Already up to date"), so **no conflict resolution has been exercised yet**. The procedure and the hotspot table above are ready. Run the first real merge as soon as upstream moves past `ebde76e9`, and record the actual conflicts here.
