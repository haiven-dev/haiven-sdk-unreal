# Unreal engine automation host

The host builds and enables the actual `unreal/Haiven` plugin. Ten unattended
engine tests cover plugin discovery/module loading; refused replies, events, and
server-initiated replies with empty request IDs; and game-login PKCE, callback
binding, one-use replay, expiry/cancellation, credential expiry, and the
no-refresh-token invariant; shared session auth, queue, and re-auth ordering;
and reflection of the subsystem plus three Blueprint async nodes. The protocol
JSON-to-frame mapping in the tests is a fixture, not a newly implemented SDK
parser.

## Provision the runners

`ci/engines.json` is the engine matrix: one row per supported version, carrying
its toolchain directory, compiler version, default install path, root
environment variable, runner label, and the `EngineAssociation` the runner
stamps into the staged host. `primary` names the version a bare
`python3 unreal/ci/run.py` builds. Adding an engine is a row there plus a
matrix entry in `.github/workflows/unreal-editor.yml`.

| Version | Toolchain | Install path | Root variable | Runner label |
| --- | --- | --- | --- | --- |
| 5.8.2 (primary) | `v26_clang-20.1.8-rockylinux8` | `/opt/unreal/5.8.2` | `UNREAL_ENGINE_ROOT_5_8` | `unreal-5.8.2-clang20` |
| 5.6.1 | `v25_clang-18.1.0-rockylinux8` | `/opt/unreal/5.6.1` | `UNREAL_ENGINE_ROOT_5_6` | `unreal-5.6.1-clang18` |

Each root must be a directory *containing* `Engine/` — that is how Epic's Linux
installed builds are packaged, and UnrealBuildTool enforces it: launched from a
path without `Engine/Binaries/DotNET` it aborts with *"This code requires that
applications using it are launched from a path containing"*. An archive
extracted a level too deep needs the enclosing directory restored, not a
symlink; .NET resolves the assembly's physical path, so a symlinked `Engine`
does not satisfy the check.

Roots resolve in order — the engine's own variable, `UNREAL_ENGINE_ROOT`, then
the install path — and a candidate whose `Build.version` is a different engine
is skipped rather than accepted, so a shell still exporting the primary root
cannot silently satisfy a run for another version. An ignored `mise.local.toml`
`[env]` entry is the usual way to point these at user-owned installations. The
runner checks `Build.version`, clang's own version string, and UBT's toolchain
selection. Ubuntu 22.04 and Python 3.10+ are the baseline; use the
engine-provided compiler/sysroot rather than system clang. Use your
organization's existing Epic entitlement to provision the engines.

Register an isolated runner per engine with labels `linux`, `x64`, and that
row's label. Allow at least 32 GB RAM and enough writable temporary space for a
clean editor-target build. Engine provisioning/build is separate from the
45-minute test job.

[`../ci/runner/Dockerfile`](../ci/runner/Dockerfile) pins the official Actions
runner image and installs the shared libraries required to launch the editor
under `-NullRHI`. Build it with `RUNNER_UID` and `RUNNER_GID` matching the owner
of the entitled engine installation. Mount the matching engine root at the
matrix path with write access: UnrealBuildTool creates engine-local state, and
UE 5.6.1 may repair the executable mode of its bundled .NET host. The Actions
work directory should be a separate writable volume; do not mount the Docker
socket or the runner operator's home directory.

The current host build and all ten automation cases passed locally on Linux on
September 18, 2026 — UE 5.8.2 with Clang 20.1.8 and UE 5.6.1 with Clang 18.1.0
from one unmodified plugin source tree. The editor emitted nonfatal
SDL/Wayland initialization warnings; all test states were `Success`, with zero
test errors and process exit code 0.

The workflow supports dispatch/reuse and does not yet run automatically on PRs.
Provision and register the CI runner, obtain its first complete passing run,
then enable PR/main triggers and require this check. Local engine proof does
not establish GitHub runner availability. Missing engines and absent reports
fail; they are never reported as passing engine coverage.

The primary target is 5.8.2; 5.6.1 is verified alongside it. Versions outside
the matrix are unverified, and adding one means adding a row and obtaining its
run, not widening a claim.

The plugin descriptor deliberately declares **no** `EngineVersion`. That field is
compared against the engine's own compatible version rather than treated as a
minimum, so any fixed value excludes every other engine: `5.8.2` made 5.8.2
itself disable the plugin (it reports 5.8.0), and `5.8.0` makes 5.6.1 refuse it
with *"The 'Haiven' plugin was designed for build 5.8.0"*. The per-engine pins
live in `ci/engines.json` instead. See
[../README.md](../README.md#supporting-more-than-one-engine-version) for the
other two version-specific settings and why `Latest` is the answer for the
target rules.

## Run from the SDK root

```sh
python3 unreal/ci/run.py            # the primary engine
python3 unreal/ci/run.py 5.6.1      # one named engine
python3 unreal/ci/run.py all        # every engine in the matrix, in order
```

`mise run test:unreal` and `mise run test:unreal:all` are the same two calls
with the engine-root environment already set. `HAIVEN_UNREAL_VERSION` is the
environment equivalent of the argument.

The harness stages a fresh host and plugin in a temporary directory, stamps the
selected engine's `EngineAssociation` into the staged `.uproject`, builds
`HaivenTestsEditor Linux Development`, checks the resulting plugin library,
and launches `UnrealEditor-Cmd` with `-unattended -NullRHI` and the `Haiven.`
automation filter. The queued `Quit` automation command exits with the test
result status once the queue drains. Build
and automation timeouts are 30 and 10 minutes; timed-out process groups are
terminated. Logs and exported JSON/HTML are retained per engine under
`build/unreal-results/<version>/` and uploaded even when tests fail.

The validator requires all ten named tests exactly once, each with `Success`,
no error events/counts, and no failed/unrun/in-progress report totals. Existing
standalone C++ checks and report-validator tests remain in `make check`, but
do not prove Unreal compilation or execution.

## Extending the engine surface

Add transport/Blueprint automation cases alongside future runtime operations,
and add their required names to `ci/engines.json`. The auth subsystem,
OpenSSL-backed PKCE, general `FHttpModule`/`IWebSocket` client, and three
Blueprint async nodes compile in both rows; the pure auth/session state machines
run inside each editor. The remaining transport qualification for the final
release item is the shared fixture/real-API pass, not another compile-only claim.

References: [UE 5.8 Linux requirements and toolchain](https://dev.epicgames.com/documentation/en-us/unreal-engine/linux-development-requirements-for-unreal-engine?application_version=5.8),
[automation CLI](https://dev.epicgames.com/documentation/en-us/unreal-engine/run-automation-tests-in-unreal-engine),
[exported result schema](https://dev.epicgames.com/documentation/unreal-engine/review-test-results-in-unreal-engine).
