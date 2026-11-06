#!/usr/bin/env python3
"""Build the real plugin in a clean host and validate exported engine reports.

One codebase, several engines. `engines.json` carries a matrix rather than a
single pin, and every version-specific value that would otherwise be baked into
a tracked file lives there: the toolchain directory, the compiler version, the
`EngineAssociation` stamped into the staged host, and the runner label CI selects
on. Nothing under `Haiven/` or `host/` names a version.

Usage:

    python3 ci/run.py            # the primary engine
    python3 ci/run.py 5.6.1      # one named engine
    python3 ci/run.py all        # every engine in the matrix, in order

`HAIVEN_UNREAL_VERSION` is the environment equivalent of the argument.
"""
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
# unreal/ci used parents[1] for the monorepo root. This file now lives in ci/.
SDK = HERE.parent
MATRIX = json.loads((HERE / "engines.json").read_text())


def label(engine):
    return ".".join(str(part) for part in engine["version"])


def select(requested):
    """Resolve a requested version to the engines to run, primary by default."""
    engines = MATRIX["engines"]
    if requested in (None, "", "primary"):
        requested = MATRIX["primary"]
    if requested == "all":
        return engines
    chosen = [engine for engine in engines if label(engine) == requested]
    if not chosen:
        known = ", ".join(label(engine) for engine in engines)
        raise RuntimeError(f"unknown engine {requested!r}; engines.json declares {known}")
    return chosen


def engine_root(engine):
    """First candidate whose Build.version actually is the requested engine.

    Order matters less than the version check does: a shell that still exports
    UNREAL_ENGINE_ROOT for the primary engine must not silently satisfy a run
    for a different one, so a candidate that does not match is skipped rather
    than accepted and reported later as a version mismatch.
    """
    candidates = [os.environ.get(engine["rootEnv"]), os.environ.get("UNREAL_ENGINE_ROOT"), engine["installPath"]]
    for candidate in candidates:
        if not candidate:
            continue
        version = Path(candidate).expanduser().resolve() / "Engine/Build/Build.version"
        if not version.is_file():
            continue
        found = json.loads(version.read_text())
        if [found.get(key) for key in ("MajorVersion", "MinorVersion", "PatchVersion")] == engine["version"]:
            return version.parents[2]
    named = ", ".join(filter(None, candidates))
    raise RuntimeError(
        f"no installed UE {label(engine)} found; set {engine['rootEnv']} to a root containing Engine/Build/Build.version"
        f" (tried: {named})")


def verify_report(path, expected):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    tests = report.get("tests", [])
    names = [test.get("fullTestPath") for test in tests]
    if not tests or len(names) != len(set(names)) or not set(expected).issubset(set(names)):
        raise RuntimeError(f"missing or duplicate expected automation tests: {names}")
    if any(report.get(field, 0) != 0 for field in ("failed", "notRun", "inProcess")):
        raise RuntimeError("automation report contains unsuccessful or incomplete tests")
    for test in tests:
        if test.get("state") != "Success" or test.get("errors", 0) != 0:
            raise RuntimeError(f"test did not pass: {test.get('fullTestPath')}: {test.get('state')}")
        if any(entry.get("event", {}).get("type") == "Error" for entry in test.get("entries", [])):
            raise RuntimeError(f"test contains error events: {test.get('fullTestPath')}")
    return names


def run(command, log, timeout, env):
    # Build.sh spawns UBT/compiler children; kill the whole process group on
    # timeout so a failed build cannot keep mutating the next runner workspace.
    with log.open("w") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   env=env, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise RuntimeError(f"timed out; see {log}")
    if code != 0:
        raise RuntimeError(f"exit {code}; see {log}")


def stage(engine, into):
    """Copy host and plugin, stamping only what is genuinely version-specific."""
    project = into / "HaivenTests"
    ignore = shutil.ignore_patterns("Binaries", "Intermediate", "Saved", "DerivedDataCache", "Plugins")
    shutil.copytree(HERE.parent / "host", project, ignore=ignore)
    shutil.copytree(HERE.parent / "Haiven", project / "Plugins/Haiven", ignore=ignore)
    fixture = project / "Content/HaivenTestFixtures/gaming-client-v0.json"
    fixture.parent.mkdir(parents=True)
    shutil.copy2(SDK / "conformance/gaming-client/v0/generated/suite.json", fixture)
    # EngineAssociation is the one host field that cannot be version-agnostic:
    # it is a single value and the editor uses it to pick an engine. The plugin
    # descriptor deliberately declares no EngineVersion and is copied unchanged.
    uproject = project / "HaivenTests.uproject"
    descriptor = json.loads(uproject.read_text())
    descriptor["EngineAssociation"] = engine["association"]
    uproject.write_text(json.dumps(descriptor, indent=2) + "\n")
    return project, uproject


def check(engine):
    version = label(engine)
    base = Path(os.environ.get("HAIVEN_UNREAL_RESULTS", SDK / "build/unreal-results")).resolve()
    results = base / version
    if results.exists(): shutil.rmtree(results)
    results.mkdir(parents=True)
    export = results / "report"
    root = engine_root(engine)
    engine_dir = root / "Engine"
    (results / "engine.json").write_text((engine_dir / "Build/Build.version").read_text())
    toolchain = engine_dir / "Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64" / engine["toolchain"]
    clang = toolchain / "x86_64-unknown-linux-gnu/bin/clang++"
    compiler_version = subprocess.check_output([str(clang), "--version"], text=True, timeout=15)
    (results / "compiler.log").write_text(compiler_version)
    if not re.search(r"clang version " + re.escape(engine["clangVersion"]) + r"\b", compiler_version):
        raise RuntimeError(f"UE {version}: bundled compiler version mismatch")
    editor = engine_dir / "Binaries/Linux/UnrealEditor-Cmd"
    if not os.access(editor, os.X_OK): raise RuntimeError(f"UE {version}: UnrealEditor-Cmd is missing or not executable")
    env = {**os.environ, "LINUX_MULTIARCH_ROOT": str(toolchain)}
    with tempfile.TemporaryDirectory(prefix=f"haiven-unreal-{version}-") as temporary:
        project, uproject = stage(engine, Path(temporary))
        run(["bash", str(engine_dir / "Build/BatchFiles/Linux/Build.sh"), MATRIX["target"], MATRIX["platform"],
             MATRIX["configuration"], str(uproject), "-WaitMutex", "-NoHotReload"], results / "build.log", 1800, env)
        build_log = (results / "build.log").read_text(errors="replace")
        if engine["toolchain"] not in build_log or engine["clangVersion"] not in build_log:
            raise RuntimeError(f"UE {version}: UBT log did not confirm the pinned toolchain")
        plugin = project / "Plugins/Haiven/Binaries/Linux/libUnrealEditor-Haiven.so"
        if not plugin.is_file(): raise RuntimeError(f"UE {version}: build did not produce the Haiven editor module")
        export.mkdir()
        # Automation Quit returns the test status; TestExit forces Unix exit 1.
        try:
            run([str(editor), str(uproject), "-unattended", "-NullRHI", "-nosplash", "-nosound",
                 "-nop4", "-stdout", "-FullStdOutLogOutput", "-ExecCmds=Automation RunTest Haiven.;Quit",
                 "-ReportExportPath=" + str(export),
                 "-abslog=" + str(results / "editor.log")], results / "automation.log", 600, env)
        finally:
            if (project / "Saved/Logs").exists():
                shutil.copytree(project / "Saved/Logs", results / "saved-logs", dirs_exist_ok=True)
        names = verify_report(export / "index.json", MATRIX["expectedTests"])
        (results / "summary.json").write_text(json.dumps(
            {"result": "Passed", "version": engine["version"], "root": str(root), "tests": names}, indent=2))
    return results


def main():
    requested = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("HAIVEN_UNREAL_VERSION")
    engines = select(requested)
    for engine in engines:
        print(f"Unreal {label(engine)}: building host and running Haiven automation", flush=True)
        results = check(engine)
        print(f"Unreal {label(engine)}: passed; results in {results}", flush=True)


if __name__ == "__main__":
    try: main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
