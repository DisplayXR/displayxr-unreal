# Packages a project into an application.
# Usage: PackageApp.py SampleProject.uproject OutputDirectory 5.2 Shipping Win64

import sys, json, winreg, os, re, glob, shutil
from distutils.dir_util import copy_tree

ProjectPath = os.path.abspath(sys.argv[1])
OutPath = os.path.abspath(sys.argv[2])
EngineVersion = sys.argv[3]
BuildType = sys.argv[4]
PlatformType = sys.argv[5]
print("Project: " + ProjectPath + " (" + BuildType + ")")
print("Output: " + OutPath)

f = open(ProjectPath, "r")

ProjectManifest = json.loads(f.read())
ProjectVersion = ProjectManifest["EngineAssociation"]
print("Project engine UE" + ProjectVersion)
print("Building against UE" + EngineVersion)

def find_engine_path(version):
    # Try the legacy registry key first (older Epic Launcher / Installed Builds).
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            "SOFTWARE\\EpicGames\\Unreal Engine\\" + version,
                            0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            return winreg.QueryValueEx(key, "InstalledDirectory")[0]
    except OSError:
        pass

    # Fall back: Epic Launcher's default install path.
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    default_path = os.path.join(program_files, "Epic Games", "UE_" + version)
    if os.path.isfile(os.path.join(default_path, "Engine", "Binaries", "Win64", "UnrealEditor.exe")):
        return default_path

    raise RuntimeError(
        "Could not find UE " + version + ": not in registry "
        "(HKLM\\SOFTWARE\\EpicGames\\Unreal Engine\\" + version + ") "
        "and not at " + default_path)

EnginePath = find_engine_path(EngineVersion)
print("Engine location: " + EnginePath)

if os.path.isdir(OutPath):
    print("Clearing output directory...")
    shutil.rmtree(OutPath)

UAT = EnginePath + '\\Engine\\Build\\BatchFiles\\RunUAT.bat'

Flags = '-project="' + ProjectPath + '" '
Flags += '-noP4 -Compressed '
Flags += '-platform=' + PlatformType + ' '
Flags += '-clientconfig=' + BuildType + ' '
Flags += '-serverconfig=' + BuildType + ' '
Flags += '-cook -allmaps -build -stage -pak -archive '
Flags += '-archivedirectory="' + OutPath + '"'

Command = '"' + UAT + '" BuildCookRun ' + Flags

# cmd /c "<cmd>" strips the outer quotes when the inner command also has
# quotes (paths with spaces, e.g. C:\Program Files\...). Wrapping the whole
# thing in an extra pair triggers cmd's "first-and-last quote stripping"
# rule, which then leaves the inner UAT path quoted exactly as intended.
print("Packaging...")
ReturnCode = os.system('"' + Command + '"')


# ---------------------------------------------------------------------------
# DisplayXR app-manifest emission (issue #5)
#
# After UAT cooks/stages, read the resolved manifest the editor wrote to
# <Project>/Saved/Config/DisplayXRManifest.json and emit:
#   1. <staged_exe>.displayxr.json   (sidecar mode, always)
#   2. %LOCALAPPDATA%\DisplayXR\apps\<sanitized>.displayxr.json  (registered
#      mode, only when register_with_displayxr is true)
# ---------------------------------------------------------------------------

def find_staged_exe(staged_root, project_name):
    """UE 5.4+ stages to <OutPath>/Windows/; older builds to /WindowsNoEditor/."""
    candidates = []
    for sub in ("Windows", "WindowsNoEditor"):
        d = os.path.join(staged_root, sub)
        if os.path.isdir(d):
            exact = os.path.join(d, project_name + ".exe")
            if os.path.isfile(exact):
                return exact
            candidates.extend(glob.glob(os.path.join(d, "*.exe")))
    if candidates:
        # Prefer the largest exe (skip small launchers/CrashReportClient).
        candidates.sort(key=lambda p: os.path.getsize(p), reverse=True)
        return candidates[0]
    return None


def build_final_manifest(resolved, exe_path, target_dir, icon_basename, icon_3d_basename):
    """Build the final launcher-discoverable manifest dict.

    - Omits optional fields at default (Unity parity).
    - Copies icon source PNGs into target_dir under the requested basenames.
    """
    out = {
        "schema_version": int(resolved.get("schema_version", 1)),
        "name": resolved["name"],
        "type": resolved.get("type", "3d"),
    }
    if exe_path:
        out["exe_path"] = exe_path

    cat = resolved.get("category", "app")
    if cat and cat != "app":
        out["category"] = cat

    dm = resolved.get("display_mode", "auto")
    if dm and dm != "auto":
        out["display_mode"] = dm

    desc = resolved.get("description", "") or ""
    if len(desc) > 256:
        desc = desc[:256]
    if desc:
        out["description"] = desc

    icon_src = resolved.get("icon_source")
    if icon_src and os.path.isfile(icon_src):
        dst = os.path.join(target_dir, icon_basename)
        shutil.copy2(icon_src, dst)
        out["icon"] = icon_basename

    icon_3d_src = resolved.get("icon_3d_source")
    if icon_3d_src and os.path.isfile(icon_3d_src):
        dst = os.path.join(target_dir, icon_3d_basename)
        shutil.copy2(icon_3d_src, dst)
        out["icon_3d"] = icon_3d_basename

    layout = resolved.get("icon_3d_layout", "sbs-lr")
    if "icon_3d" in out and layout and layout != "sbs-lr":
        out["icon_3d_layout"] = layout

    return out


if ReturnCode == 0:
    manifest_path = os.path.join(os.path.dirname(ProjectPath), "Saved", "Config", "DisplayXRManifest.json")
    if not os.path.isfile(manifest_path):
        print("No DisplayXRManifest.json found at " + manifest_path + " - skipping manifest emit")
    else:
        with open(manifest_path, "r") as mf:
            resolved = json.load(mf)

        project_name = os.path.splitext(os.path.basename(ProjectPath))[0]
        staged_exe = find_staged_exe(OutPath, project_name)
        if not staged_exe:
            print("WARN: could not locate staged .exe under " + OutPath + " - skipping manifest emit")
        else:
            staged_dir = os.path.dirname(staged_exe)

            # 1. Sidecar manifest (always)
            sidecar = build_final_manifest(
                resolved,
                exe_path=None,
                target_dir=staged_dir,
                icon_basename="icon.png",
                icon_3d_basename="icon_3d.png",
            )
            sidecar_path = staged_exe + ".displayxr.json"
            with open(sidecar_path, "w") as sf:
                json.dump(sidecar, sf, indent=2)
            print("Wrote sidecar manifest: " + sidecar_path)

            # 2. Registered manifest (optional)
            if resolved.get("register_with_displayxr"):
                local_appdata = os.environ.get("LOCALAPPDATA")
                if not local_appdata:
                    print("WARN: LOCALAPPDATA not set - skipping registered manifest")
                else:
                    apps_dir = os.path.join(local_appdata, "DisplayXR", "apps")
                    os.makedirs(apps_dir, exist_ok=True)
                    sanitized = re.sub(r"[^A-Za-z0-9._-]", "_", resolved["name"])
                    registered = build_final_manifest(
                        resolved,
                        exe_path=staged_exe,
                        target_dir=apps_dir,
                        icon_basename=sanitized + ".png",
                        icon_3d_basename=sanitized + "_sbs.png",
                    )
                    reg_path = os.path.join(apps_dir, sanitized + ".displayxr.json")
                    with open(reg_path, "w") as rf:
                        json.dump(registered, rf, indent=2)
                    print("Wrote registered manifest: " + reg_path)

if ReturnCode == 0 and os.path.isdir(OutPath):
    directories = [f.path for f in os.scandir(OutPath) if f.is_dir()]
    for directory in directories:
        print("Exporting redist to " + directory)
        copy_tree(EnginePath + "\\Engine\\Extras\\Redist", directory + "\\Engine\\Extras\\Redist")

sys.exit(ReturnCode)
