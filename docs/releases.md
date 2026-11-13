# Releases

`.github/workflows/release.yml` publishes a GitHub release when a `v*`
tag is pushed. `ci/release.py` builds every asset. The same script runs
locally. `.github/workflows/source-hosted.yml` checks that source on
GitHub-hosted Linux, Windows, and macOS (protocol compile, descriptor,
release-script tests). The release workflow calls it before the Linux
binary jobs. It does not build plugin binaries.

## Cutting a release

1. On `main`, set `VersionName` in `Haiven/Haiven.uplugin` to the release
   version and increment the integer `Version` by one. The workflow
   accepts a tag only when it is `v` plus that `VersionName`.
2. Tag that commit and push the tag:
   `git tag v1.2.3 && git push origin v1.2.3`.

Dispatching the workflow with no tag is a dry run: it builds the assets
at the branch's `VersionName` and publishes nothing.

## Assets

Each asset is a zip whose only top-level entry is `Haiven/`, for
`<Project>/Plugins/`:

- `haiven-unreal-plugin-source-<version>.zip` — plugin source, without
  `Binaries` or `Intermediate`. It builds against the engines in
  `ci/engines.json`.
- `haiven-unreal-plugin-<version>-UE<major.minor>-Linux.zip` —
  `RunUAT.sh BuildPlugin` output for one engine, with prebuilt
  `Binaries/Linux` and `EngineVersion` pinned to that engine.
- `SHA256SUMS` covering those zips.

## Platforms

Win64 and Mac binaries are not built. `BuildPlugin` for Win64 needs
`RunUAT.bat` on Windows, and Mac needs `RunUAT.sh` plus Xcode.
GitHub-hosted runners do not include Unreal Engine, and this repository
does not contain the engine, so those platforms have no hosted binary
job. A self-hosted Windows or macOS runner is not wired in either: a
missing label would queue the release until timeout and block the Linux
zips. Linux binaries stay on the self-hosted runners named in
`ci/engines.json`.

## Runner safety

The binary job runs on self-hosted runners. Do not add a `pull_request`
or `pull_request_target` trigger to `release.yml`. This repository is
public, and a pull-request trigger would run fork code on those runners.
Source checks on pull requests run from `source-hosted.yml`, which has no
self-hosted runner.
