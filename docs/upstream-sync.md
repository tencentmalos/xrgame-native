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
| `app/build.gradle.kts` | WP0: `picoXr` flavor, source set, `xrgame` signing config. WP1: `XRGAME` BuildConfig field. The picoXr variant filter, release signing and the cargo-ndk `libgndownload` task sit in one block at the end of the file | 23 | 182 |
| `ubuntufs/build.gradle.kts` | WP0: `picoXr` flavor and variant filter | 0 | 6 |
| `.gitmodules` | Setup: `references/*` entries appended at the end | 0 | 2 |
| `gradle/libs.versions.toml` | WP1: `javasteam` = `1.8.0.1-26-xrg.<gitlink>`. When upstream bumps its SNAPSHOT, advance `references/JavaSteam` to the matching commit and keep our fixed-version form | 6 | 78 |
| `settings.gradle.kts` | WP1: Sonatype snapshots replaced by the `build/javasteam-maven` exclusive repository | 0 | 8 |
| `app/src/main/java/app/gamenative/PluviaApp.kt` | WP1: one call, `XrGame.install(this)`, before `NetworkMonitor.init` | 9 | 39 |
| `app/src/main/java/app/gamenative/ui/screen/library/appscreen/SteamAppScreen.kt` | WP1: the storage-permission shortcut `MODERN_ANDROID -> true` excludes `XRGAME` | 6 | 67 |
| `app/src/main/AndroidManifest.xml` | None so far; picoXr manifest entries go in `src/picoXr/AndroidManifest.xml` | 10 | 47 |
| `app/src/main/java/app/gamenative/service/SteamService.kt` | None so far. If a WP has to touch it, prefer a small hook over inline edits | 27 | 221 |
| `app/src/main/java/app/gamenative/PrefManager.kt` | None so far. Same rule as `SteamService.kt` | 30 | 114 |

Churn for the two WP1 files was measured the same way on 2026-09-24.

Our new files do not exist upstream and cannot conflict: `src/picoXr/**`, `src/testPicoXr/**`, `src/main/java/app/gamenative/xrgame/**`, `src/main/res/values*/strings_xrgame.xml`, `tools/build-javasteam.*`, `tools/javasteam/**`, `.github/workflows/xrgame-picoxr.yml`, `docs/**`, `references/**`, `AGENTS.md` and `CLAUDE.md`.

## Log

### 2026-09-24: first drill (no upstream delta)

- `git fetch upstream` at 2026-09-24: `upstream/master` = `ebde76e9`, the fork base. `git merge-base --is-ancestor upstream/master malos/main` succeeds, and `git log ebde76e9..upstream/master` is empty.
- A merge would be a no-op ("Already up to date"), so **no conflict resolution has been exercised yet**. The procedure and the hotspot table above are ready. Run the first real merge as soon as upstream moves past `ebde76e9`, and record the actual conflicts here.
