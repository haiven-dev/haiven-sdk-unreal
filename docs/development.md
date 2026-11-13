# Development

## Checks without Unreal

From the repository root, with no Unreal install:

```sh
./verify.sh
node validate-plugin.mjs
```

`verify.sh` builds `protocol_test.cpp` as C++17 with `-Wall -Wextra
-Werror` and runs it. That covers the protocol and session headers only.
If neither `g++` nor `clang++` is on `PATH`, the script prints
`skipping C++: no g++ or clang++` and exits 0. That skip is not a passing
protocol run.

`node validate-plugin.mjs` checks the plugin descriptor and the build
rules. It does not load the plugin in an editor.

## Engine automation

With an entitled engine whose root directory contains `Engine/`:

```sh
python3 ci/run.py            # UE 5.8.2
python3 ci/run.py 5.6.1
python3 ci/run.py all
```

Set `UNREAL_ENGINE_ROOT_5_8` or `UNREAL_ENGINE_ROOT_5_6` to that root.
`UNREAL_ENGINE_ROOT` is also consulted, and a root for a different
version is skipped. `HAIVEN_UNREAL_VERSION` selects the same engine as
the argument. The runner stages `host/` and the plugin, stamps
`EngineAssociation`, builds `HaivenTestsEditor`, and runs the `Haiven.`
automation filter. The case list and the runner image are in
[host/README.md](../host/README.md). Those editor tests do not open a live
HTTP or WebSocket fixture.

How one source tree builds on several engines is in
[engine-versions.md](engine-versions.md).

## Conformance

`conformance_test.cpp` drives `Haiven::FSessionCore` against the suite
at `conformance/gaming-client/v0/generated/suite.json`. It is not part
of `verify.sh`. It prints a skip and exits 0 unless
`HAIVEN_CONFORMANCE_SUITE` (that file's path),
`HAIVEN_FIXTURE_BASE_URL`, and `HAIVEN_FIXTURE_WS_URL` are set.
