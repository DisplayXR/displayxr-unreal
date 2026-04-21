# TODO

Outstanding work for the DisplayXR Unreal plugin. Organized by theme, not priority — grab whichever unlocks your next milestone.

---

## 1. In-flight work

### Native XR editor preview
Replace the current `SceneCapture2D`-based editor preview with a `FDisplayXRDevice` → PIE hookup so UE renders directly into the OpenXR swapchain in the editor (zero-copy, UMG/HUD supported).

- Motivation: remove duplicate render path, pick up TAA history / eye adaptation / post-process state automatically, include UI widgets in the preview.
- Design + agent prompt already drafted; not yet merged into this repo. When picked up, port `EditorPreviewNative.md` + `EditorPreviewNative-AgentPrompt.md` into `Docs/DisplayXR/`.
- Key hypothesis to re-verify: UE's PIE pipeline does not invoke `FDisplayXRDevice::UpdateViewport()`. Every working UE XR plugin (OpenXR, SteamVR, Meta) activates in PIE, so the hook pattern exists — it was either missed or the device isn't registered in a way UE recognizes for PIE. First step: instrument `EnableStereo`, `IsStereoEnabled`, `ShouldUseSeparateRenderTarget`, `GetIdealRenderTargetSize`, `AllocateRenderTargetTexture`, `BeginRenderViewFamily`, compare sequence against Epic's `OpenXRHMD`.

---

## 2. Missing design docs (referenced elsewhere)

- `Architecture.md` — module layout, class hierarchy, data flow, dual-path design (UE OpenXR on Windows/Android vs direct session on macOS). Unity has `docs/architecture/` split into `hook-chain.md`, `kooima-pipeline.md`, `preview-session.md`. Match that split here.
- `OpenXRIntegration.md` — `IOpenXRExtensionPlugin` hook, `xrLocateViews` interception, per-frame sequence diagram.

---

## 3. Parity with [displayxr-unity](https://github.com/DisplayXR/displayxr-unity)

Unity sibling has a more mature docs tree. Bring Unreal up to parity so users can switch between plugins without surprises.

- **`docs/quick-start-guide.md`** — step-by-step: install → demo scene camera-centric → demo scene display-centric → editor test → Windows build → macOS build → cross-compile. Unity version is ~470 lines. Adapt for UE (uplugin install, Project Settings, Blueprint rig setup, UAT `BuildCookRun`).
- **`CHANGELOG.md`** — Keep-a-Changelog format. Seed with `## [0.1.0]` summarizing the initial DisplayXR release.
- **`Docs/DisplayXR/adr/`** — Unity has 6 ADRs covering deferred destruction, dual session, native preview window, camera-vs-display mode, multipass-forced, window-relative Kooima. Port the ones that apply to Unreal; write new ADRs where the UE port made different calls.
- **`Docs/DisplayXR/architecture/`** — subfolder with `hook-chain.md` (UE: `IOpenXRExtensionPlugin` + `FSceneViewExtensionBase`), `kooima-pipeline.md` (shared C lib), `preview-session.md` (current + native XR preview plan).
- **`Docs/DisplayXR/roadmap/`** — carry over Unity planning docs relevant to shared native code (`full-gfx-backends-refactor-plan.md`, `phase1-mac-finish.md`). Add UE-specific roadmap items.
- **Top-level README restructure** — match Unity's TOC style, add a "New to the plugin?" callout block, link to a sibling `displayxr-unreal-test` minimal UE project (if/when one exists).
- **Companion test project** — create `DisplayXR/displayxr-unreal-test` mirroring `DisplayXR/displayxr-unity-test`: a minimal UE project with this plugin preconfigured, two demo maps (camera-centric, display-centric), ready to open and hit Play.

---

## 4. CI & release

- **Real UE build in CI** — current `lint.yml` only does vendor-name + JSON/YAML checks. Full compile needs either a self-hosted runner with UE preinstalled or Epic's container registry (requires Epic account linkage). Matrix target: UE 5.3–5.6.
- **Release workflow** — package `DisplayXR.uplugin` per UE version, upload to GitHub Releases. Replaces any prior internal upload flow.
- **Tagged versioning** — `VersionName` in `DisplayXR.uplugin` should be kept in sync with git tags (e.g. `v0.1.0`).

---

## 5. Platform coverage

- **macOS path validation** — `DISPLAYXR_USE_UNREAL_OPENXR=0`, direct session via `FDisplayXRDirectSession`. End-to-end smoke test on a supported display.
- **Android path validation** — OpenXR hook on current DisplayXR runtime. Vulkan backend.
- **UE version sweep** — currently targets UE 5.3+. Confirm 5.6 works; track 5.7 pre-release.

---

## 6. Known issues / cleanup

- Kooima C libs (`camera3d_view.c/h`, `display3d_view.c/h`) are shared with `displayxr-unity`. Keep them engine-agnostic — no UE or Unity types — and sync changes both directions.
- Some file-header years are `2025-2026`; new files should use `2026-` or extend the range as appropriate.
