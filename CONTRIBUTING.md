# Contributing to displayxr-unreal

The DisplayXR Unreal plugin — Kooima eye-tracked stereo rendering for
OpenXR-compatible 3D displays. C++ source compiled by
UnrealBuildTool, no native side-build separately.

See the [org-wide CONTRIBUTING](https://github.com/DisplayXR/.github/blob/main/CONTRIBUTING.md)
for issue routing, branch/PR flow, and licensing. The notes below cover
the Unreal-specific build/test loop.

## Repo layout

- **`Source/DisplayXRCore/`** — runtime module: OpenXR session, atlas
  handoff, Kooima projection, eye-tracking integration.
- **`Source/DisplayXREditor/`** — editor module: standalone OpenXR
  preview session, project-settings UI.
- **`Source/DisplayXRMaterials/`** — shaders and material assets.
- **`Config/`** — `DefaultDisplayXR.ini` etc.
- **`Resources/`** — plugin icon and other UE-required assets.
- **`Docs/DisplayXR/`** — architecture, atlas-handoff, editor-preview
  docs.

`DisplayXR.uplugin` is the descriptor UnrealBuildTool reads.

## Building

The plugin builds as part of the host project's UBT pass. There's no
standalone `build` script — drop the plugin into a UE 5.3+ project's
`Plugins/` directory (or symlink it) and build the host project
normally.

For a quick build-and-test loop:

1. Clone or open the test project with this plugin under its
   `Plugins/DisplayXR/`.
2. Generate project files: right-click the `.uproject` → *Generate
   Visual Studio project files* (Windows) or run the equivalent
   `UnrealBuildTool` command on macOS / Linux.
3. Build via Visual Studio / Xcode.
4. Open the `.uproject` and hit Play in Editor (PIE).

Eye tracking + Kooima projection require the DisplayXR OpenXR runtime
to be installed and active.

## CI

Only `lint.yml` runs in CI on PRs — UE doesn't have a free-tier headless
build path that's lightweight enough for per-PR runs. Lint catches
clang-tidy / formatting / header inclusion issues. Full builds happen
on dev machines + via the test project against tagged plugin releases.

## Conventions

- **C++ style** — Unreal coding standard. PascalCase for
  classes/methods, camelCase for locals, `b` prefix for bool members,
  `F`/`U`/`A`/`I` prefixes for struct / object / actor / interface
  classes.
- **Module boundaries** — keep editor-only code in
  `DisplayXREditor`. The runtime module ships in cooked games; the
  editor module does not.
- **Unreal API targeting** — UE 5.3 is the floor. If you need a newer
  API, gate it with `#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= N)`.
- **Atlas handoff** — never weave; always hand the atlas off to the
  display processor. See [`Docs/DisplayXR/AtlasHandoff.md`](Docs/DisplayXR/AtlasHandoff.md)
  and [ADR-007 in the runtime repo](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/adr/ADR-007-compositor-never-weaves.md).

## Architecture

[`Docs/DisplayXR/Architecture.md`](Docs/DisplayXR/Architecture.md) is
the entry point. Key supporting docs:

- [`AtlasHandoff.md`](Docs/DisplayXR/AtlasHandoff.md) — zero-copy hand-off
  to the runtime's display processor.
- [`EditorPreview.md`](Docs/DisplayXR/EditorPreview.md) — standalone
  OpenXR session in the editor.
- [`DisplayRigSetup.md`](Docs/DisplayXR/DisplayRigSetup.md) — pawn /
  camera setup for stereo rendering.
- [`TODO.md`](Docs/DisplayXR/TODO.md) — outstanding work and Unity-parity
  gaps.

For OpenXR extension semantics consumed by this plugin (display info,
window binding, eye tracking modes), see the
[runtime extension specs](https://github.com/DisplayXR/displayxr-runtime/tree/main/docs/specs).

## Licensing

Boost Software License 1.0. By contributing you agree your work is
licensed under BSL-1.0.
