# Contributing

Contributions are welcome. This repository is public. Bug reports, fixes,
and documentation improvements are all useful.

## Issues

Open a GitHub issue for a bug, a question about behavior, or a change you
want to discuss before writing it. Search open issues first.

Include the plugin version, the Unreal Engine version, and the steps that
show the problem. Do not paste access tokens, passwords, or private
endpoints.

## Pull requests

1. Fork the repository and branch from `main`.
2. Keep the change focused.
3. Use a [Conventional Commits](https://www.conventionalcommits.org/)
   subject:

   `type(scope): subject`

   `type` is one of `feat`, `fix`, `docs`, `deps`, `style`, `refactor`,
   `perf`, `test`, `build`, `ci`, `chore`, or `revert`. `scope` is
   optional.
4. Open the pull request against `main`. Say what changed and how you
   checked it.

Do not commit access tokens, passwords, private endpoints, or local
credentials.

## Checks

From the repository root, without an Unreal install:

```sh
./verify.sh
node validate-plugin.mjs
```

`verify.sh` compiles and runs the engine-free protocol and session test.
The validator checks the plugin descriptor and build rules only. It is
not a substitute for loading the plugin in an editor.

Engine automation is under `host/`. The versions this repository lists
are Unreal Engine 5.8.2 and 5.6.1 (`ci/engines.json`). Say which version
you used.
