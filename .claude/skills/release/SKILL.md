---
name: release
description: Create a tagged release of the DisplayXR Unreal plugin. Bumps the uplugin version, commits + tags, packages the plugin locally via PackagePlugin.bat, and publishes a GitHub Release with the packaged ZIP attached. Use /release v1.0.0 for explicit version, or /release patch|minor|major for auto-bump.
allowed-tools: Read, Grep, Glob, Bash, Agent, Edit, Write
---

# Release Skill — DisplayXR Unreal Plugin

Creates a tagged release of the Unreal plugin, packages it locally (no CI), and publishes a GitHub Release with the packaged ZIP asset attached.

## Architecture

```
/release v0.2.0
  │
  ├─ Pre-flight checks (clean tree, on main, version valid, no existing tag/release)
  ├─ Bump VersionName in DisplayXR.uplugin
  ├─ Prepend CHANGELOG.md entry
  ├─ Commit + create tag + push
  ├─ Build locally:
  │    ├─ Scripts\PackagePlugin.bat 5.7  →  Packages\DisplayXR_5.7\
  │    └─ Compress-Archive               →  DisplayXR_5.7.zip
  ├─ gh release create v0.2.0 DisplayXR_5.7.zip
  ├─ Verify asset attached
  └─ Report
```

**Key difference from the Unity sibling skill:** the Unreal plugin is packaged **locally on the dev machine**, not in CI. There is no workflow to monitor — `PackagePlugin.bat` runs synchronously here. The dev machine must have Unreal Engine 5.7 installed and Visual Studio build tools available.

## CRITICAL: Launch Subagent

**You MUST use the Agent tool with `subagent_type="general-purpose"` to execute this workflow.** The subagent carries out every step below in order and reports back.

### Parsing the Version Argument

Parse `[ARGUMENTS]` to determine version:

1. If argument matches `vN.N.N` → use as explicit version.
2. If argument is `patch` → read current `VersionName` from `DisplayXR.uplugin`, bump patch (0.7.0 → 0.7.1).
3. If argument is `minor` → bump minor (0.7.0 → 0.8.0).
4. If argument is `major` → bump major (0.7.0 → 1.0.0).
5. If no argument → ask user what version.

The git tag uses the `v` prefix (`v0.2.0`); the `VersionName` field in `DisplayXR.uplugin` does NOT (`0.2.0`).

### Subagent Prompt Template

Replace `[VERSION]` with the resolved version (e.g., `v0.2.0`) and `[VERSION_NUMBER]` with the version without the `v` prefix (e.g., `0.2.0`):

```
Execute the DisplayXR Unreal plugin release workflow for version [VERSION].

## Configuration
- Repo: DisplayXR/displayxr-unreal
- Version source of truth: DisplayXR.uplugin "VersionName" field
- Git tag format: v{major}.{minor}.{patch}
- UE version targeted: 5.7
- Release asset: DisplayXR_5.7.zip
- Package script: Scripts\PackagePlugin.bat
- Package output dir: Packages\DisplayXR_5.7\

---

## PHASE 1: PRE-FLIGHT CHECKS

### Step 1.1: Verify clean state
Run: `git status --short`
- If dirty, report and STOP: "Working tree is not clean. Commit or stash changes first."

### Step 1.2: Verify on main branch
Run: `git branch --show-current`
- If not `main`, report and STOP: "Must be on main branch to release."

### Step 1.3: Verify tag doesn't already exist
Run: `git fetch --tags origin` then `git tag -l "[VERSION]"`
- If tag exists, report and STOP: "Tag [VERSION] already exists."

### Step 1.4: Verify GitHub Release doesn't already exist
Run: `gh release view [VERSION] --repo DisplayXR/displayxr-unreal 2>&1 || true`
- If it exits 0 (release exists), report and STOP: "GitHub Release [VERSION] already exists."

### Step 1.5: Verify DisplayXR.uplugin current version
Read `DisplayXR.uplugin`. Extract `"VersionName": "X.Y.Z"`.
- Confirm the resolved [VERSION_NUMBER] is strictly greater than current.

### Step 1.6: Get previous tag for release notes
Run: `git tag --sort=-v:refname | grep '^v' | head -1`
Store as PREV_TAG. (May be empty for the first release.)

---

## PHASE 2: UPDATE VERSION AND CHANGELOG

### Step 2.1: Bump DisplayXR.uplugin VersionName
Use Edit tool to change `"VersionName": "X.Y.Z"` → `"VersionName": "[VERSION_NUMBER]"`.

### Step 2.2: Add CHANGELOG.md entry
Read CHANGELOG.md. Find the top of the version-entry area (after the file header + intro, before the first `## [` line).

Generate commit summary since PREV_TAG (or since repo start if PREV_TAG is empty):
```bash
git log PREV_TAG..HEAD --oneline --no-merges
```
Group commits by prefix (feat/fix/docs/refactor/ci) and prepend a new section to CHANGELOG.md:
```
## [[VERSION_NUMBER]] - YYYY-MM-DD

### Added
- ...

### Fixed
- ...

### Changed
- ...
```
Use today's date. If unsure about grouping, just list all commits under `### Changed`.

### Step 2.3: Commit version bump
```bash
git add DisplayXR.uplugin CHANGELOG.md
git commit -m "Release [VERSION]"
```
Store the commit SHA: `git rev-parse HEAD`.

### Step 2.4: Create tag and push
```bash
git tag [VERSION]
git push origin main
git push origin [VERSION]
```

---

## PHASE 3: BUILD LOCALLY

### Step 3.1: Package the plugin
```bash
Scripts\PackagePlugin.bat 5.7
```
This invokes UAT BuildPlugin and writes to `Packages\DisplayXR_5.7\`. Takes several minutes. Check exit code.
- On failure → go to PHASE 5 (Rollback), include the tail of UAT output.

### Step 3.2: Strip binaries we don't want to ship (optional)
Remove `Packages\DisplayXR_5.7\Intermediate\` if UAT left it behind — the shipped ZIP should only contain source + `Binaries/Win64/*`, `Resources/`, `Config/`, `DisplayXR.uplugin`.

### Step 3.3: Zip the packaged plugin
Use pwsh:
```powershell
Compress-Archive -Path Packages\DisplayXR_5.7\* -DestinationPath DisplayXR_5.7.zip -Force
```

---

## PHASE 4: PUBLISH GITHUB RELEASE

### Step 4.1: Extract release notes
Find the `## [[VERSION_NUMBER]] - ...` section in CHANGELOG.md and write its body (up to the next `## [` or EOF) to `RELEASE_BODY.md` at repo root (temp file).

### Step 4.2: Create the release
```bash
gh release create [VERSION] DisplayXR_5.7.zip \
  --repo DisplayXR/displayxr-unreal \
  --title "DisplayXR [VERSION]" \
  --notes-file RELEASE_BODY.md
```

### Step 4.3: Verify
```bash
gh release view [VERSION] --repo DisplayXR/displayxr-unreal --json tagName,name,assets
```
Confirm the `DisplayXR_5.7.zip` asset is listed.

### Step 4.4: Cleanup
Remove `RELEASE_BODY.md` and `DisplayXR_5.7.zip` from the working tree (both are gitignored).

### Step 4.5: Report
```
DisplayXR Unreal [VERSION] published!

Packaged locally:
  - UE version: 5.7
  - Output:     Packages\DisplayXR_5.7\
  - Asset:      DisplayXR_5.7.zip

Published to:
  - GitHub Release: https://github.com/DisplayXR/displayxr-unreal/releases/tag/[VERSION]

Consume in a test project:
  1. Set .displayxr-version to [VERSION]
  2. Run: pwsh Scripts\fetch-plugin.ps1

Changelog:
  [generated changelog summary]
```

STOP.

---

## PHASE 5: ROLLBACK (on failure)

### Step 5.1: Delete remote and local tag
```bash
git push --delete origin [VERSION] 2>&1 || true
git tag -d [VERSION] 2>&1 || true
```

### Step 5.2: Revert version-bump commit
```bash
git revert HEAD --no-edit
git push origin main
```

### Step 5.3: Delete GH release if it was created
```bash
gh release delete [VERSION] --repo DisplayXR/displayxr-unreal --yes 2>&1 || true
```

### Step 5.4: Report failure
```
DisplayXR Unreal [VERSION] release FAILED — rolled back.

Failure stage: [phase and step where it broke]
Error:
[error tail]

Tag, version bump, and CHANGELOG entry have been reverted.
Fix the issue and try again with /release [VERSION]
```

STOP.
```
