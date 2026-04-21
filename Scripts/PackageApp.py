# Packages a project into an application.
# Usage: PackageApp.py SampleProject.uproject OutputDirectory 5.2 Shipping Win64

import sys, json, winreg, os, shutil
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

EnginePath = ""

try:
    EngineKey = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, "SOFTWARE\\EpicGames\\Unreal Engine\\" + EngineVersion, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    EnginePath = installed = winreg.QueryValueEx(EngineKey, "InstalledDirectory")[0]
    winreg.CloseKey(EngineKey)
except Exception as ex:
    print("Could not find UE" + EngineVersion + "(" + ex + ")")
    raise

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

Command = UAT + ' BuildCookRun ' + Flags

print("Packaging...")
ReturnCode = os.system(Command)

directories = [f.path for f in os.scandir(OutPath) if f.is_dir()]
for directory in directories:
    print("Exporting redist to " + directory)
    copy_tree(EnginePath + "\\Engine\\Extras\\Redist", directory + "\\Engine\\Extras\\Redist")

sys.exit(ReturnCode)
