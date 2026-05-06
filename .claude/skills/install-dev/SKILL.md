---
name: install-dev
description: Install displayxr-unreal-test in dev mode for this plugin checkout. Clones the test project into a target folder (default ~/Documents/Unreal) and creates a Windows junction from <test>/Plugins/DisplayXR -> this displayxr-unreal checkout, so plugin edits flow directly into the UE editor without a release re-fetch. Use /install-dev [target-parent-dir].
allowed-tools: Read, Grep, Glob, Bash, Edit, Write
---

# Install-Dev Skill — DisplayXR Unreal Plugin

Wire up `displayxr-unreal-test` as a running host for **this** plugin checkout, bypassing the release-ZIP fetch. Use this when you are actively developing the plugin and want the UE editor to pick up your local source on rebuild.

## When to use this skill vs. the verify-install agent

| You want to...                                   | Use                                                     |
|--------------------------------------------------|---------------------------------------------------------|
| Develop the plugin (edit `Source/`, rebuild)     | **this skill** — `/install-dev`                         |
| Validate a cut release tag end-to-end            | `agents/verify-install.md` in `displayxr-unreal-test`   |

The two are mutually exclusive on disk: `Plugins/DisplayXR/` in the test project is either a release-ZIP extraction or a junction, not both. Switching modes is documented at the end of this file.

## Architecture

```
/install-dev [target-parent-dir]
  │
  ├─ Pre-flight (UE 5.7, VS 2022 + NetFxSDK, runtime JSON, gh auth)
  ├─ Mirror plugin to no-space dev root if PLUGIN_SRC or TARGET_PARENT has spaces
  ├─ Clone displayxr-unreal-test into <target>/displayxr-unreal-test
  ├─ cmd /c mklink /J <target>/displayxr-unreal-test/Plugins/DisplayXR  <plugin (or mirror)>
  ├─ Generate Visual Studio project files for DisplayXRTest.uproject
  ├─ Build DisplayXRTestEditor (Win64 Development) — first build is 10–20 min
  └─ Report: sln path, junction source, next step (launch editor)
```

## Arguments

`[ARGUMENTS]` — optional parent directory for the test-project clone. Default `$HOME\Documents\Unreal`. The test repo lands at `<arg>\displayxr-unreal-test\`.

## Prerequisites — verify before acting; halt with `PREREQ_MISSING` if any are absent

- **Windows 10/11** with `cmd.exe` available (for `mklink /J`).
- **Unreal Engine 5.7** installed. Default path `C:\Program Files\Epic Games\UE_5.7\`. Verify by checking `Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe` exists.
- **Visual Studio 2022** with the "Game development with C++" workload **including the .NET Framework 4.6.2+ SDK and matching targeting pack**. Verify by registry: `HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\NETFXSDK` (or the non-WOW6432Node equivalent) must exist. Missing this causes UBT to fail with `Could not find NetFxSDK install dir` before any plugin code compiles — install the SDK and targeting pack from the VS Installer → Individual components.
- **DisplayXR runtime** installed: `C:\Program Files\DisplayXR\Runtime\DisplayXR_win64.json` exists.
- **GitHub CLI** authenticated: `gh auth status` exits 0.
- **Spaced paths are auto-mitigated, not rejected.** UE 5.7's `ContentBrowserAssetDataSource` fatals when the project path contains spaces — it mixes 8.3 and long-filename probes when mounting `Content/Developers/`. The skill works around this by mirroring `PLUGIN_SRC` to a no-space dev root (default `C:\dxr-dev\`) and switching `TARGET_PARENT` to the same root if `$HOME` straddles the bug. Mechanism is in Step 1.
- **Working tree on `PLUGIN_SRC` must be clean if a mirror is needed.** The mirror is a fresh `git clone` of the same branch — uncommitted edits in the original checkout would not appear in the mirror, and the user would think they were testing code they hadn't actually saved. Halt with `PREREQ_MISSING` and ask them to commit or stash first. (When `PLUGIN_SRC` has no spaces, no mirror is created and this check is skipped.)

## Steps — stop on first failure

### 1. Resolve paths and mirror plugin if needed

- `TARGET_PARENT = [ARGUMENTS] ?? "$HOME\Documents\Unreal"`. If the resolved value contains a space (e.g., `$HOME` is `C:\Users\Jane Doe`), substitute `C:\dxr-dev\` instead. The user-supplied argument is honored verbatim; reject with `PREREQ_MISSING` if the user explicitly passed a spaced path.
- `PLUGIN_SRC = git -C . rev-parse --show-toplevel` (convert forward slashes to backslashes for `mklink`).
- **If `PLUGIN_SRC` contains a space** — auto-mirror to a no-space dev root:
  - Verify clean working tree: `git -C "$PLUGIN_SRC" status --porcelain` must be empty. If dirty, halt with `PREREQ_MISSING` and ask the user to commit or stash first. The mirror is a fresh clone — uncommitted edits would silently disappear from the build.
  - Capture branch and origin: `BRANCH = git -C "$PLUGIN_SRC" branch --show-current`; `ORIGIN = git -C "$PLUGIN_SRC" remote get-url origin`.
  - `MIRROR_DIR = "C:\dxr-dev\displayxr-unreal"`.
  - If `MIRROR_DIR` already exists: ask the user whether to **reuse** as-is, **refresh** (`git -C "$MIRROR_DIR" fetch origin && git -C "$MIRROR_DIR" reset --hard origin/$BRANCH`), or **abort**. Do not silently overwrite.
  - Otherwise create `C:\dxr-dev\` if missing, then clone: `git clone -b $BRANCH $ORIGIN "$MIRROR_DIR"`.
  - Reassign `PLUGIN_SRC = $MIRROR_DIR` for the rest of the steps. Build artifacts (`Binaries/`, `Intermediate/`) will land in the mirror, which is what we want — they would otherwise pollute the original spaced checkout.
  - Surface the mirror prominently in the final report so the user knows future edits must happen in `$MIRROR_DIR`, not the original.
- `TEST_PROJECT = "$TARGET_PARENT\displayxr-unreal-test"`.
- If `TEST_PROJECT` already exists and is non-empty, stop and **ask the user** whether to delete (show the exact command: `Remove-Item -Recurse -Force "$TEST_PROJECT"`) or abort. Do not silently overwrite.
- Create `TARGET_PARENT` if missing.

### 2. Clone the test project

```bash
git clone https://github.com/DisplayXR/displayxr-unreal-test.git "$TEST_PROJECT"
```

Expect `.displayxr-version` and `DisplayXRTest.uproject` at the clone root. If the clone fails (likely: network or auth), stop and include `gh auth status` output alongside the git error in the report.

### 3. Create the junction

**Do NOT run `Scripts\fetch-plugin.ps1`** — dev mode replaces the plugin fetch with a junction to this repo.

```powershell
cmd /c mklink /J "$TEST_PROJECT\Plugins\DisplayXR" "$PLUGIN_SRC"
```

`mklink /J` creates a directory junction: no admin required, resolves transparently for Unreal/UBT, and `Plugins/` is already gitignored in the test repo so nothing leaks into commits. Build output written under `Plugins/DisplayXR/Binaries/` and `.../Intermediate/` lands inside this plugin checkout (both paths are gitignored here too) — which is what you want while debugging the plugin.

Verify:

```powershell
Test-Path "$TEST_PROJECT\Plugins\DisplayXR\DisplayXR.uplugin"
```

Must return `True`. If it's `False`, the junction did not resolve — stop and report.

### 4. Generate Visual Studio project files

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" `
  -projectfiles `
  -project="$TEST_PROJECT\DisplayXRTest.uproject" `
  -game -rocket -progress
```

The last line before `Total execution time` must be `Result: Succeeded`. Verify `$TEST_PROJECT\DisplayXRTest.sln` was created.

If UBT logs `Unable to instantiate module 'SwarmInterface': Could not find NetFxSDK install dir` here, the NetFxSDK prereq check in step 0 missed it — reclassify as `PREREQ_MISSING`, not a skill bug.

### 5. Build the Development Editor target

First build is 10–20 min (shader compile + full engine module link against the plugin). Use a **45-minute timeout**. Subsequent incremental builds finish in seconds.

Invoke `Build.bat` via PowerShell — the space in the UE install path makes direct Bash invocation brittle:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  DisplayXRTestEditor Win64 Development `
  -project="$TEST_PROJECT\DisplayXRTest.uproject" -WaitMutex
```

The last line before `Total execution time` must be `Result: Succeeded`.

If the build fails with plugin compile errors (missing includes, unresolved symbols, type mismatches in `Source/`), **that is legitimate dev-loop signal, not a skill failure** — dev mode is pointing UE at your in-flight plugin source. Report the error tail and stop; the user will fix the source in `$PLUGIN_SRC\Source\` and re-run the build (either `Build.bat` again, or F5 in Visual Studio). The junction and sln remain valid across rebuilds.

### 6. Report

```
Dev install complete.

Test project:   <TEST_PROJECT>
Plugin source:  <PLUGIN_SRC>
  (junctioned at <TEST_PROJECT>\Plugins\DisplayXR)
Solution:       <TEST_PROJECT>\DisplayXRTest.sln

DisplayXRTestEditor built successfully (Win64 Development).

[If the plugin was mirrored due to spaces in the original checkout, also include:]
  ⚠ Plugin was mirrored from <ORIGINAL_SPACED_PATH> to <MIRROR_DIR>.
    Future edits MUST be made in <MIRROR_DIR>, not the original — UE
    only sees the mirror through the junction. The original checkout
    is now stale and should not be used for plugin development.

Launch the editor:

  & "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" `
    "<TEST_PROJECT>\DisplayXRTest.uproject"

Or: open <TEST_PROJECT>\DisplayXRTest.sln in Visual Studio, set
DisplayXRTestEditor as startup project, F5.

Edits to <PLUGIN_SRC>\Source\ are picked up on the next rebuild
(Build.bat again, or incremental compile in Visual Studio).
```

## Switching back to release mode

To drop dev mode and test the pinned release tag instead:

```powershell
cmd /c rmdir "<TEST_PROJECT>\Plugins\DisplayXR"
cd "<TEST_PROJECT>"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts\fetch-plugin.ps1
```

`cmd /c rmdir` on a junction deletes only the link — the displayxr-unreal source repo is untouched. Prefer this form over `Remove-Item -Recurse` because it is unambiguous across shell versions regarding junction traversal.

## Notes

- The junction is local-only. Nothing about it is tracked in either repo.
- If you later move or rename this `displayxr-unreal` directory, the junction becomes a dangling link. Delete and recreate it after the move.
- Running this skill a second time against an existing test-project clone should surface the "already exists" prompt in step 1. If you want a clean re-install, delete first (the command is in the prompt), then re-run.
