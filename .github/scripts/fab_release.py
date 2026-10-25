#!/usr/bin/env python3
"""Check the MantleCore plugin against Fab's rules and package the Fab zip.

The release workflow (.github/workflows/release.yml) runs this on a GitHub-hosted ubuntu runner.
Run it locally the same way before you tag:

    python .github/scripts/fab_release.py check [--tag v0.1.0]
    python .github/scripts/fab_release.py package [--out dist]

check runs every rule below on the plugin folder and exits 1 if any fails:
  version   the tag (when given) is "v" + VersionName from MantleCore.uplugin
  uplugin   EngineVersion 5.8.0, PlatformAllowList ["Win64"] on every module, CreatedBy Joshua Eger,
            FabURL is the live Fab listing (Fab Technical Requirements 4.3.6.a, 4.3.6.b and 4.3.6.c)
  headers   every source file starts with the copyright line (4.3.6.1.b); same rules as the game
            repo's tools/copies/check_headers.py
  content   Content holds only IK rig and IK retargeter assets: no textures, materials, material
            instances, material functions or physical materials (owner rule, issue 950)
  licenses  every library under Source/ThirdParty has its LICENSE file

package writes MantleCore-<VersionName>-UE5.8.zip: the plugin as source under a top-level
MantleCore/ folder, without Binaries, Intermediate, Saved, DerivedDataCache or dot folders such
as .git, .github and .claude (4.3.7.3.a). It then opens the zip, checks its layout, unpacks it and
runs every check above again on what it holds, so the checks cover exactly what ships.

Exit codes: 0 all checks pass, 1 a check failed, 2 bad arguments or the plugin was not found.

Python 3.9+, standard library only.
"""

import argparse
import json
import os
import re
import sys
import tempfile
import zipfile
from pathlib import Path

PLUGIN_NAME = "MantleCore"
ENGINE_VERSION = "5.8.0"
ENGINE_LABEL = "UE5.8"
PLATFORMS = ["Win64"]
CREATED_BY = "Joshua Eger"
FAB_LISTING_ID = "0ab720ef-e3da-4dc7-81ee-7eafb573c6bc"
FAB_URL = f"com.epicgames.launcher://ue/Fab/product/{FAB_LISTING_ID}"

# Folders never shipped, anywhere in the tree, plus every file or folder whose name starts with "."
# (.git, .github, .claude, .gitattributes, .gitignore, .vs).
EXCLUDED_DIRS = {"Binaries", "Intermediate", "Saved", "DerivedDataCache"}

# Header rules, ported from mantle's tools/copies/check_headers.py.
HEADER = "// Copyright Joshua Eger. All Rights Reserved."
HEADER_EXTENSIONS = {".h", ".cpp", ".inl", ".c", ".cs"}
BOM = b"\xef\xbb\xbf"

# Content rules. The editor writes each asset's class path ("/Script/<Module>.<Class>") into the
# package's asset registry data, so the class can be read without Unreal.
ALLOWED_ASSET_CLASSES = {"/Script/IKRig.IKRigDefinition", "/Script/IKRig.IKRetargeter"}
CLASS_PATH = re.compile(rb"/Script/[A-Za-z0-9_]+\.[A-Za-z0-9_]+")
PACKAGE_TAG = bytes.fromhex("c1832a9e")  # 0x9E2A83C1, little endian: every .uasset starts with it
LFS_POINTER = b"version https://git-lfs"
# Usual name prefixes of textures, materials, material instances and functions, physical materials.
FORBIDDEN_PREFIXES = ("T_", "TX_", "M_", "MI_", "MF_", "MFI_", "MM_", "PM_", "PhysMat_")

LICENSE_NAMES = {"LICENSE", "LICENSE.txt", "LICENSE.md"}


def shipped(relative):
    """True for a path (relative to the plugin, forward slashes) that belongs in the Fab zip."""
    parts = relative.split("/")
    return not any(part in EXCLUDED_DIRS or part.startswith(".") for part in parts)


def plugin_files(plugin):
    """Every file under the plugin folder that ships, as forward-slash relative paths, sorted."""
    result = []
    for root, dirs, files in os.walk(plugin):
        rel_root = Path(root).relative_to(plugin).as_posix()
        rel_root = "" if rel_root == "." else rel_root + "/"
        dirs[:] = sorted(d for d in dirs if shipped(rel_root + d))
        result += [rel_root + name for name in files if shipped(rel_root + name)]
    return sorted(result)


def read_uplugin(plugin):
    path = plugin / f"{PLUGIN_NAME}.uplugin"
    return json.loads(path.read_text(encoding="utf-8-sig"))


# --- checks: each returns a list of problems (empty = pass) and a one-line summary -----------------

def check_version(plugin, tag):
    version = read_uplugin(plugin).get("VersionName", "")
    if not version:
        return ["MantleCore.uplugin has no VersionName"], ""
    if tag is None:
        return [], f"VersionName {version} (no tag given, not compared)"
    if tag != f"v{version}":
        return [f"tag {tag} does not match VersionName {version} in MantleCore.uplugin (expected tag v{version})"], ""
    return [], f"tag {tag} matches VersionName {version}"


def check_uplugin(plugin, _tag):
    data = read_uplugin(plugin)
    problems = []
    if data.get("EngineVersion") != ENGINE_VERSION:
        problems.append(f"EngineVersion is {data.get('EngineVersion')!r}, expected {ENGINE_VERSION!r}")
    if data.get("CreatedBy") != CREATED_BY:
        problems.append(f"CreatedBy is {data.get('CreatedBy')!r}, expected {CREATED_BY!r}")
    if data.get("FabURL") != FAB_URL:
        problems.append(f"FabURL is {data.get('FabURL')!r}, expected {FAB_URL!r}")
    modules = data.get("Modules") or []
    if not modules:
        problems.append("no Modules listed")
    for module in modules:
        if module.get("PlatformAllowList") != PLATFORMS:
            problems.append(f"module {module.get('Name')}: PlatformAllowList is "
                            f"{module.get('PlatformAllowList')!r}, expected {PLATFORMS!r}")
    return problems, f"EngineVersion {ENGINE_VERSION}, CreatedBy {CREATED_BY}, FabURL set, {len(modules)} modules Win64 only"


def header_in_scope(relative):
    """True for a path under Source (relative to it) that needs the header."""
    parts = relative.split("/")
    if Path(relative).suffix not in HEADER_EXTENSIONS:
        return False  # non-code files, such as ThirdParty/TypeScriptCompiler/typescript.js
    # Vendored upstream code keeps its own license header: Source/ThirdParty/<library>/upstream/...
    if len(parts) > 3 and parts[0] == "ThirdParty" and parts[2] == "upstream":
        return False
    return True


def first_line(raw):
    body = raw[len(BOM):] if raw.startswith(BOM) else raw
    return body.split(b"\n", 1)[0].rstrip(b"\r")


def check_headers(plugin, _tag):
    files = [rel[len("Source/"):] for rel in plugin_files(plugin) if rel.startswith("Source/")]
    targets = [rel for rel in files if header_in_scope(rel)]
    problems = [f"Source/{rel}: first line is not \"{HEADER}\""
                for rel in targets if first_line((plugin / "Source" / rel).read_bytes()) != HEADER.encode()]
    return problems, f"{len(targets)} source files start with the copyright line"


def check_content(plugin, _tag):
    files = [rel for rel in plugin_files(plugin) if rel.startswith("Content/")]
    problems = []
    for rel in files:
        name = Path(rel).name
        if Path(rel).suffix != ".uasset":
            problems.append(f"{rel}: only .uasset files may be in Content")
            continue
        if name.startswith(FORBIDDEN_PREFIXES):
            problems.append(f"{rel}: named like a texture or material ({name.split('_')[0]}_ prefix)")
        raw = (plugin / rel).read_bytes()
        if raw.startswith(LFS_POINTER):
            problems.append(f"{rel}: is a Git LFS pointer, not the asset (check out with LFS)")
            continue
        if not raw.startswith(PACKAGE_TAG):
            problems.append(f"{rel}: is not an Unreal package")
            continue
        classes = sorted(set(m.decode() for m in CLASS_PATH.findall(raw)))
        if not classes:
            problems.append(f"{rel}: asset class not found")
        for cls in classes:
            if cls not in ALLOWED_ASSET_CLASSES:
                problems.append(f"{rel}: asset class {cls} is not allowed "
                                f"(only {', '.join(sorted(ALLOWED_ASSET_CLASSES))})")
    if not files:
        problems.append("Content is empty or missing (expected the IK rig and IK retargeter assets)")
    return problems, f"{len(files)} assets, all IK rigs or IK retargeters"


def check_licenses(plugin, _tag):
    third_party = plugin / "Source" / "ThirdParty"
    libraries = sorted(p for p in third_party.iterdir() if p.is_dir()) if third_party.is_dir() else []
    problems = []
    for library in libraries:
        licenses = [p for p in library.iterdir() if p.is_file() and p.name in LICENSE_NAMES and p.stat().st_size > 0]
        if not licenses:
            problems.append(f"Source/ThirdParty/{library.name}: no LICENSE file")
    return problems, f"{len(libraries)} third-party libraries have a LICENSE ({', '.join(p.name for p in libraries)})"


CHECKS = [
    ("version", check_version),
    ("uplugin", check_uplugin),
    ("headers", check_headers),
    ("content", check_content),
    ("licenses", check_licenses),
]


def run_checks(plugin, tag):
    """Run every check, print the results, return the number that failed."""
    failed = 0
    for name, check in CHECKS:
        problems, summary = check(plugin, tag)
        if problems:
            failed += 1
            print(f"FAILED {name}: {len(problems)} problem(s)")
            for problem in problems:
                print(f"  {problem}")
        else:
            print(f"OK     {name}: {summary}")
    return failed


# --- packaging -------------------------------------------------------------------------------------

def zip_name(plugin):
    return f"{PLUGIN_NAME}-{read_uplugin(plugin)['VersionName']}-{ENGINE_LABEL}.zip"


def write_zip(plugin, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / zip_name(plugin)
    # Leave out the output folder when it sits inside the plugin, so an earlier zip never ships.
    files = [rel for rel in plugin_files(plugin) if out_dir.resolve() not in (plugin / rel).parents]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for rel in files:
            # A fixed timestamp keeps the zip the same for the same files.
            info = zipfile.ZipInfo(f"{PLUGIN_NAME}/{rel}", date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, (plugin / rel).read_bytes())
    return path, len(files)


def check_zip_layout(path):
    problems = []
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
    for name in names:
        if not name.startswith(f"{PLUGIN_NAME}/"):
            problems.append(f"{name}: not under the top-level {PLUGIN_NAME}/ folder")
        elif not shipped(name[len(PLUGIN_NAME) + 1:]):
            problems.append(f"{name}: must not ship (Binaries, Intermediate, Saved, DerivedDataCache or a dot file)")
    for required in (f"{PLUGIN_NAME}/{PLUGIN_NAME}.uplugin", f"{PLUGIN_NAME}/Source/", f"{PLUGIN_NAME}/Content/"):
        if not any(name == required or name.startswith(required) for name in names):
            problems.append(f"missing {required}")
    return problems, names


def command_check(args):
    plugin = args.plugin
    failed = run_checks(plugin, args.tag)
    if failed:
        print(f"FAILED {failed} of {len(CHECKS)} checks")
        return 1
    print(f"OK     all {len(CHECKS)} checks pass")
    return 0


def command_package(args):
    plugin = args.plugin
    path, count = write_zip(plugin, args.out)
    print(f"wrote {path} ({count} files, {path.stat().st_size:,} bytes)")

    problems, names = check_zip_layout(path)
    top = sorted({name.split("/")[1] for name in names if name.count("/") >= 1})
    if problems:
        print(f"FAILED zip layout: {len(problems)} problem(s)")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print(f"OK     zip layout: everything under {PLUGIN_NAME}/ ({', '.join(top)})")

    with tempfile.TemporaryDirectory() as temp:
        with zipfile.ZipFile(path) as archive:
            archive.extractall(temp)
        print(f"checking the zip's contents:")
        failed = run_checks(Path(temp) / PLUGIN_NAME, args.tag)
    if failed:
        print(f"FAILED {failed} of {len(CHECKS)} checks on the zip's contents")
        return 1

    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as out:
            out.write(f"zip={path.as_posix()}\nname={path.name}\nversion={read_uplugin(plugin)['VersionName']}\n")
    print(f"OK     {path.name} is ready for Fab")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check the MantleCore plugin and package the Fab zip.")
    default_plugin = Path(__file__).resolve().parents[2]
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "package"):
        command = sub.add_parser(name)
        command.add_argument("--plugin", type=Path, default=default_plugin,
                             help="plugin folder (default: this repo)")
        command.add_argument("--tag", help="release tag to compare with VersionName, such as v0.1.0")
        if name == "package":
            command.add_argument("--out", type=Path, default=Path("dist"), help="folder for the zip (default: dist)")
    args = parser.parse_args(argv)

    args.plugin = args.plugin.resolve()
    if not (args.plugin / f"{PLUGIN_NAME}.uplugin").is_file():
        print(f"ERROR {args.plugin} has no {PLUGIN_NAME}.uplugin")
        return 2
    return command_check(args) if args.command == "check" else command_package(args)


if __name__ == "__main__":
    sys.exit(main())
