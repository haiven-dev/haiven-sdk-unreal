#!/usr/bin/env python3
"""Build the release assets that `.github/workflows/release.yml` publishes.

Every asset is a zip whose single top-level entry is a `Haiven/` plugin folder,
so a game drops it into `<Project>/Plugins/` unchanged.

Usage:

    python3 ci/release.py version [--tag v1.2.3]
        Print the release version. With --tag, the tag must be `v` + the
        `VersionName` in `Haiven/Haiven.uplugin`, or this exits non-zero.
        Without it, the descriptor's `VersionName` is the version (dry runs).

    python3 ci/release.py matrix
        Print the binary build matrix derived from `ci/engines.json`, as the
        JSON `strategy.matrix` value the workflow consumes.

    python3 ci/release.py source --version 1.2.3 --out dist/
        Write `haiven-unreal-plugin-source-<version>.zip`: the plugin as it is
        in the repository, without Binaries/Intermediate/Saved/DerivedDataCache.

    python3 ci/release.py binary --engine 5.8.2 --version 1.2.3 --out dist/
        Run the engine's `RunUAT.sh BuildPlugin` for Linux and write
        `haiven-unreal-plugin-<version>-UE<major.minor>-Linux.zip`, which
        carries the prebuilt `Binaries/Linux` next to `Source/`. The UAT log
        goes to --logs (default `build/release-logs/`), never into --out.

Only stdlib Python. The engine lookup and the bundled-toolchain environment are
the same ones `ci/run.py` uses, so a runner that passes the editor job builds
release binaries with the same compiler.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
PLUGIN = REPO / "Haiven"
DESCRIPTOR = PLUGIN / "Haiven.uplugin"

# Build products and editor state that must never ship in a source bundle.
EXCLUDED_DIRS = {"Binaries", "Intermediate", "Saved", "DerivedDataCache"}
# Platforms the self-hosted runners can build today. Win64 needs a Windows host
# with the engine installed (RunUAT.bat); Mac needs a macOS host with Xcode.
SUPPORTED_PLATFORMS = ("Linux",)
# Semantic version with an optional pre-release suffix: 1.2.3 or 1.2.3-rc.1.
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?$")
# Fixed timestamp so the same tree always produces the same zip bytes.
EPOCH = (1980, 1, 1, 0, 0, 0)
# BuildPlugin compiles editor and game targets for each platform; generous,
# because a cold runner also builds the host project UBT creates for it.
BUILD_TIMEOUT_SECONDS = 3600


def load_runner():
    spec = importlib.util.spec_from_file_location("unreal_runner", HERE / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def descriptor_version():
    return json.loads(DESCRIPTOR.read_text(encoding="utf-8-sig"))["VersionName"]


def release_version(tag=None):
    declared = descriptor_version()
    if not VERSION.match(declared):
        raise RuntimeError(f"{DESCRIPTOR.name} VersionName {declared!r} is not a semantic version")
    if not tag:
        return declared
    if not tag.startswith("v") or not VERSION.match(tag[1:]):
        raise RuntimeError(f"tag {tag!r} is not v<major>.<minor>.<patch>[-prerelease]")
    if tag[1:] != declared:
        raise RuntimeError(
            f"tag {tag} does not match {DESCRIPTOR.name} VersionName {declared!r}; "
            f"bump VersionName to {tag[1:]!r} on main before tagging")
    return declared


def matrix(platform="Linux"):
    runner = load_runner()
    include = []
    for engine in runner.MATRIX["engines"]:
        include.append({
            "version": runner.label(engine),
            "ue": engine["association"],
            "label": engine["runnerLabel"],
            "root": engine["installPath"],
            "platform": platform,
        })
    return {"include": include}


def plugin_files(root):
    """Every file under `root`, sorted, skipping excluded directories at any depth."""
    files = []
    for directory, subdirectories, names in os.walk(root):
        subdirectories[:] = sorted(name for name in subdirectories if name not in EXCLUDED_DIRS)
        for name in sorted(names):
            files.append(Path(directory) / name)
    return sorted(files, key=lambda path: path.relative_to(root).as_posix())


def write_zip(root, archive, skip_excluded=True):
    """Zip `root` as a top-level `Haiven/` folder, deterministically."""
    archive.parent.mkdir(parents=True, exist_ok=True)
    if skip_excluded:
        files = plugin_files(root)
    else:
        files = sorted((path for path in root.rglob("*") if path.is_file()),
                       key=lambda path: path.relative_to(root).as_posix())
    if not files:
        raise RuntimeError(f"nothing to package under {root}")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in files:
            entry = zipfile.ZipInfo("Haiven/" + path.relative_to(root).as_posix(), date_time=EPOCH)
            entry.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if os.access(path, os.X_OK) else 0o644
            entry.external_attr = (0o100000 | mode) << 16
            bundle.writestr(entry, path.read_bytes())
    return archive


def checked(version):
    if not VERSION.match(version):
        raise RuntimeError(f"version {version!r} is not <major>.<minor>.<patch>[-prerelease]")
    return version


def source(version, out):
    version = checked(version)
    return write_zip(PLUGIN, Path(out).resolve() / f"haiven-unreal-plugin-source-{version}.zip")


def stamp_engine_version(descriptor, association):
    """Pin a packaged descriptor to the engine its binaries were built with.

    The tracked descriptor omits EngineVersion on purpose (validate-plugin.mjs
    requires that) so one source tree loads in every engine. Prebuilt binaries
    are the opposite case: they only load in the engine that built them, so the
    packaged copy must say so. BuildPlugin normally stamps `<major>.<minor>.0`;
    this keeps that, fills it when absent, and refuses any other value.
    """
    expected = f"{association}.0"
    data = json.loads(descriptor.read_text(encoding="utf-8-sig"))
    found = data.get("EngineVersion")
    if found not in (None, expected):
        raise RuntimeError(f"packaged {descriptor.name} declares EngineVersion {found!r}, expected {expected!r}")
    if found is None:
        data["EngineVersion"] = expected
        descriptor.write_text(json.dumps(data, indent="\t") + "\n", encoding="utf-8")
    return expected


def binary(requested, version, out, logs, platform="Linux"):
    version = checked(version)
    if platform not in SUPPORTED_PLATFORMS:
        raise RuntimeError(
            f"platform {platform!r} is not buildable here; this script drives RunUAT.sh on a Linux host "
            f"({', '.join(SUPPORTED_PLATFORMS)} only)")
    runner = load_runner()
    (engine,) = runner.select(requested)
    root = runner.engine_root(engine)
    engine_dir = root / "Engine"
    toolchain = engine_dir / "Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64" / engine["toolchain"]
    if not toolchain.is_dir():
        raise RuntimeError(f"UE {runner.label(engine)}: bundled toolchain {engine['toolchain']} is missing")
    uat = engine_dir / "Build/BatchFiles/RunUAT.sh"
    if not uat.is_file():
        raise RuntimeError(f"UE {runner.label(engine)}: {uat} is missing")
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    logs = Path(logs).resolve()
    logs.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "LINUX_MULTIARCH_ROOT": str(toolchain)}
    with tempfile.TemporaryDirectory(prefix=f"haiven-release-{runner.label(engine)}-") as temporary:
        # BuildPlugin wants a pristine source plugin outside the package folder,
        # so build from a copy: a stray Binaries/ in the checkout cannot leak in.
        staged = Path(temporary) / "src/Haiven"
        shutil.copytree(PLUGIN, staged, ignore=shutil.ignore_patterns(*EXCLUDED_DIRS))
        package = Path(temporary) / "package/Haiven"
        runner.run(["bash", str(uat), "BuildPlugin",
                    f"-Plugin={staged / DESCRIPTOR.name}",
                    f"-Package={package}",
                    f"-TargetPlatforms={platform}",
                    "-Rocket", "-NoP4"],
                   logs / f"buildplugin-UE{engine['association']}-{platform}.log", BUILD_TIMEOUT_SECONDS, env)
        module = package / "Binaries" / platform / "libUnrealEditor-Haiven.so"
        if not module.is_file():
            raise RuntimeError(f"UE {runner.label(engine)}: BuildPlugin did not produce {module.name}")
        # BuildPlugin leaves its intermediate host project inside the package.
        shutil.rmtree(package / "HostProject", ignore_errors=True)
        stamp_engine_version(package / DESCRIPTOR.name, engine["association"])
        archive = out / f"haiven-unreal-plugin-{version}-UE{engine['association']}-{platform}.zip"
        return write_zip(package, archive, skip_excluded=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    version_parser = commands.add_parser("version")
    version_parser.add_argument("--tag")
    commands.add_parser("matrix")
    source_parser = commands.add_parser("source")
    source_parser.add_argument("--version", required=True)
    source_parser.add_argument("--out", required=True)
    binary_parser = commands.add_parser("binary")
    binary_parser.add_argument("--engine", required=True)
    binary_parser.add_argument("--version", required=True)
    binary_parser.add_argument("--out", required=True)
    binary_parser.add_argument("--logs", default=str(REPO / "build/release-logs"))
    binary_parser.add_argument("--platform", default="Linux")
    args = parser.parse_args(argv)
    if args.command == "version":
        print(release_version(args.tag))
    elif args.command == "matrix":
        print(json.dumps(matrix(), separators=(",", ":")))
    elif args.command == "source":
        print(source(args.version, args.out))
    elif args.command == "binary":
        print(binary(args.engine, args.version, args.out, args.logs, args.platform))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        sys.exit(str(error))
