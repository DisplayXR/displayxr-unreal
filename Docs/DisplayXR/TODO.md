# TODO

Outstanding work for the DisplayXR Unreal plugin. Organized by theme, not priority — grab whichever unlocks your next milestone.

---

## 1. In-flight work

### Native XR editor preview
Replace the current `SceneCapture2D`-based editor preview with a `FDisplayXRDevice` → PIE hookup so UE renders directly into the OpenXR swapchain in the editor (zero-copy, UMG/HUD supported).

- Motivation: remove duplicate render path, pick up TAA history / eye adaptation / post-process state automatically, include UI widgets in the preview.
- Plan: [`EditorPreviewNative.md`](./EditorPreviewNative.md) (5-phase investigation). Agent handoff prompt: [`EditorPreviewNative-AgentPrompt.md`](./EditorPreviewNative-AgentPrompt.md).

---

## 2. Parity with [displayxr-unity](https://github.com/DisplayXR/displayxr-unity)

The Unity sibling has a more mature docs tree. Bring Unreal up to parity so users can switch between plugins without surprises.

### Done
- ✅ Quick-start guide → [`QuickStart.md`](./QuickStart.md)
- ✅ Single-page architecture doc → [`Architecture.md`](./Architecture.md)
- ✅ First 3 ADRs (Unreal-specific: direct runtime loading, zero-copy atlas, UE-native off-axis projection) → [`adr/`](./adr/)

### Still open

- **`CHANGELOG.md`** — Keep-a-Changelog format. Seed with `## [0.1.0]` summarizing the initial DisplayXR release.
- **Additional ADRs mapped from Unity's set** — Unity has 6 ADRs covering decisions that are likely to have Unreal-side analogues or counter-decisions. Write a matching Unreal ADR (or a "does not apply because X" stub) for each:
  - Unity ADR-001 *deferred-destruction* — Unity-specific teardown ordering; check if UE's module shutdown path has the same trap.
  - Unity ADR-002 *dual-session* — Unity runs two OpenXR sessions (game + preview). We also do this via `FDisplayXRPreviewSession`; capture the Unreal-side tradeoffs.
  - Unity ADR-003 *native-preview-window* — Relevant to current `FDisplayXRPreviewSession`; describe the Unreal window-creation path.
  - Unity ADR-004 *camera-vs-display-mode* — We ship both `UDisplayXRCamera` and `UDisplayXRDisplay`; record the selection criteria.
  - Unity ADR-005 *multipass-forced* — UE has separate multipass/instanced/MMV stereo modes; document which we force and why.
  - Unity ADR-006 *window-relative-kooima* — Relevant for display-centric math; record how the Unreal port handles window-rect vs monitor-rect.
- **`Docs/DisplayXR/architecture/` subfolder** (optional depth docs) — Unity has `hook-chain.md`, `kooima-pipeline.md`, `preview-session.md`. Our current [`Architecture.md`](./Architecture.md) is one page; split if it grows past that. Kooima pipeline detail already lives in [`EyeTracking.md`](./EyeTracking.md); preview-session detail is split across [`EditorPreview.md`](./EditorPreview.md) and [`EditorPreviewNative.md`](./EditorPreviewNative.md).
- **`Docs/DisplayXR/roadmap/`** — Unity has planning docs (`full-gfx-backends-refactor-plan.md`, `phase1-mac-finish.md`, etc.). Carry over the ones relevant to shared native code; add UE-specific roadmap items.
- **Top-level README polish** — current [`README.md`](../../README.md) has a TOC and the "New here?" callout. Unity's README is richer (per-section screenshots, deployment, troubleshooting). Bring the density up once we have a companion test project to link from.
- **Companion test project** — create `DisplayXR/displayxr-unreal-test` mirroring `DisplayXR/displayxr-unity-test`: a minimal UE project with this plugin preconfigured, two demo maps (camera-centric, display-centric), ready to open and hit Play.

---

## 3. CI & release

- **Real UE build in CI** — current `lint.yml` only does vendor-name + JSON/YAML checks. Full compile needs either a self-hosted runner with UE preinstalled or Epic's container registry (requires Epic account linkage). Matrix target: UE 5.3–5.6.
- **Release workflow** — package `DisplayXR.uplugin` per UE version, upload to GitHub Releases. Replaces any prior internal upload flow.
- **Tagged versioning** — `VersionName` in `DisplayXR.uplugin` should be kept in sync with git tags (e.g. `v0.1.0`).

---

## 4. Platform coverage

- **macOS path validation** — end-to-end smoke test of the unified `FDisplayXRSession` (Metal graphics binding, Cocoa window binding) on a supported display. See [MacSetup.md](./MacSetup.md).
- **Android path validation** — unified session on Android (Vulkan graphics binding).
- **UE version sweep** — currently targets UE 5.3+. Confirm 5.6 works; track 5.7 pre-release.

---

## 5. Known issues / cleanup

- Kooima C libs (`camera3d_view.c/h`, `display3d_view.c/h`) are shared with `displayxr-unity`. Keep them engine-agnostic — no UE or Unity types — and sync changes both directions.
- Some file-header years are `2025-2026`; new files should use `2026-` or extend the range as appropriate.
