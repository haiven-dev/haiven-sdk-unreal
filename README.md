# Unreal spike — the C++ binding

## The question

**Does the outbound envelope's discriminant survive into C++, and can an Unreal
binding be written so that a machine without Unreal can still check the part
where the bug lives?**

The first half is F10 asked for the third time. C# needed `bool?` plus an
explicit presence check, because a non-nullable `bool` maps an absent field to
`false` and turns every event into a failed reply. GDScript needed `has()`,
because `get()` answers `null` and `null` is falsy. C++ has neither of those
languages' excuses and the same trap available, so it is worth measuring rather
than predicting.

The second half is a reaction to **F20**. The Unity binding's WebGL transport
turned out to be the only file in this repository that nothing compiled —
written, reviewed, and never once put through a compiler, because it needed
Unity to build. An Unreal module written the idiomatic way, in `FString` and
`TSharedPtr<FJsonObject>`, would have been that file again and several hundred
lines larger.

## What has been answered

**The discriminant needs `std::optional<bool>`, and nothing weaker.** A plain
`bool` cannot hold "absent", so it maps an event to `false` — the C# collapse
exactly, in a language with no dynamic typing to blame it on. Three bindings,
three languages, the same mistake available and idiomatic in each. It is a
property of the discriminant, not of any language.

**So it is encoded rather than documented.** `HaivenProtocol.h` carries a
`static_assert` that fails if `Success` is ever narrowed to `bool`, with the
reason in the message. Verified by doing it: the assertion fires first, ahead of
the cascade of `has_value` errors that would otherwise be the only signal and
would read as a refactor to finish rather than a decision to reverse.

**The protocol core takes no engine types, and that is the point.** It is plain
C++17 — `std::optional`, `std::string` — so `verify.sh` compiles and exercises it
with an ordinary `g++`/`clang++` at `-Wall -Wextra -Werror`. The cost is real and
worth stating rather than hiding: those types are unidiomatic in an Unreal
module, and every value crossing into the engine layer pays a conversion. What
it buys is that the classification logic — the part with the bug in it — is
checkable at all, on a machine with no engine installed.

## Game login handoff

`UHaivenSubsystem` now owns the game login transaction and exchanged access
token for one game instance. `BeginGameLogin` creates independent 256-bit state
and PKCE verifier values with OpenSSL's platform CSPRNG, stores the verifier only
in memory, opens the authorization URL through `FPlatformProcess::LaunchURL`,
and returns the URL for hosts that also need to display or record it.

Route the platform deep-link callback to `HandleGameLoginCallback`. The
subsystem validates the exact redirect scheme, authority, path, configured query
and state before consuming the verifier. It then posts exactly `code`,
`code_verifier`, `integration_id`, and `redirect_uri` as
`application/x-www-form-urlencoded` through `FHttpModule`. Completion and
failure delegates are always dispatched to the game thread and are assignable
from Blueprints.

The stored credential is deliberately memory-only and contains the access token,
token type, and absolute expiry. `GetAccessToken` refuses an expired credential;
`Logout` clears it. The handoff has no client secret, refresh operation, or
refresh-token property, and a response containing `refresh_token` is rejected.
Games that require persistence must copy the credential into platform secure
storage and rehydrate their own token source; the plugin does not use config
files or `USaveGame` as a secret store.

The callback entry point is the cross-platform adapter boundary because Unreal
does not expose one portable deep-link delegate across desktop and mobile. Call
it from the platform lifecycle integration that receives the registered redirect
scheme. Cancellation clears transient verifier state and cancels an in-flight
HTTP exchange without producing a stale Blueprint completion.

## Runtime client

`UHaivenSubsystem` is now the game-instance owner for all three runtime
boundaries. `Request`/`RequestAsync` use `FHttpModule`, attach the current bearer
and `X-Haiven-SDK`, return a cancellation handle, and surface one result on the
game thread. `ConnectRealtime` creates an engine `IWebSocket`; the plain-C++
`FSessionCore` sends auth first, queues application frames until `auth.success`,
correlates replies, replays unanswered frames with their original IDs, and reads
the token owner again for same-socket re-auth.

The split is deliberate. Unreal owns JSON conversion, `IWebSocket`, and
`AsyncTask(ENamedThreads::GameThread, ...)`; `FSessionCore` owns protocol order
and contains no engine type, so `verify.sh` exercises the behavior with an
ordinary C++17 compiler. A raw-frame multicast remains available because auth
frames are consumed into semantic state and therefore do not reach the
reply/event arms.

C++ callers can choose completion delegates or `TFuture` for HTTP, authenticated
connect, and correlated realtime requests. Blueprints get three
`UBlueprintAsyncActionBase` nodes for the same operations, each with an explicit
cancel entry point. `SetAccessToken` is the host rehydration boundary for a
bearer restored from platform secure storage; the plugin still does not persist
credentials itself.

## Running it

```
make check        # from the repository root
```

Skips loudly without a C++ compiler.

## Conformance

`conformance_test.cpp` executes the published `gaming/v0` suite against the real
`cmd/wsfixture`, driven by `scripts/check-unreal-conformance.py` and wired into
`make check` through `check-unreal`. It drives `Haiven::FSessionCore` — the same
core `HaivenNetworking.cpp` drives — rather than a raw socket, because the
question is whether the binding can be driven by the step vocabulary, not
whether the server answers; the node and dotnet runners already answer that from
a bare transcript client.

`gaming/v0/profiles/unreal.yaml` names `runner: unreal-automation` and requires
`engine-independent-protocol-core`. That second requirement is what runs here.
An `FAutomationTestBase` case would need an engine, and the machines this
repository is checked on do not have one — so a conformance runner written that
way would be the Unity WebGL transport again: written, reviewed, never executed.
The suite reader and the transport are therefore plain C++17 over POSIX sockets,
in `conformance/Json.h` and `conformance/Transport.h`, with no third-party
header and nothing on the compiler's command line but `-std=c++17`.

Three things this runner established, each measured rather than argued:

**The raw arm is the only one that can observe the handshake, in C++ as in
GDScript.** `FSessionCore` consumes `auth.success` and `auth.expired` into
semantic state, so they never reach `OnReply`/`OnEvent`. Pointing the runner's
inbox at those arms instead fails all four scenarios, and every failure reads
`expected a auth.success and nothing arrived` — indistinguishable from a server
that sent nothing. Third language, same finding.

**`connect` and `auth` are separable here, and were not in Godot.** The addon is
handed a token callable and handshakes itself the moment the socket opens, so
`godot/conformance_test.gd` has to defer `connect` and do the work at `auth`,
and refuses any corpus shape needing them apart. `FSessionCore` does not:
`Opened()` is an explicit call, so the socket can be open with nothing written
on it, and the runner asserts exactly that between the two steps. The corpus's
two-step shape needs no adaptation in C++ — and the reason is the property the
profile requires, a core the *runner* drives rather than one the transport
drives.

**The auth payload is a JSON string and every other payload is an object.**
`WriteClientFrame` special-cases `auth` and the runner restates it; serializing
uniformly authenticates nobody and reads as a server rejecting valid tokens.

Two guards, both proven non-vacuous. `check-unreal-conformance.py` recomputes
the expected scenario count from the suite document rather than trusting the
runner's self-report: making the runner also claim `deployment` drops it to a
green 2-scenario run, and the stage fails on the count instead of accepting it.
And the stage skips loudly — no compiler, no vendored suite, no backend checkout
and no Go each name themselves, because a suite whose absence is
indistinguishable from its success is not a suite.

Network-adapter execution *through the engine* remains the open case: `host/`
builds and ticks under both pinned engines but has never contacted a live
fixture. `#sdkunrealeditorci` owns the provisioned runners that would change
that.

## Engine-backed harness

`host/` supplies a minimal project plus plugin/module, protocol, session, and game-login
automation tests. See [host/README.md](host/README.md) for the engine matrix
runner and strict report checks. GitHub runner provisioning remains separate
from this local engine proof.

## Supporting more than one engine version

One source tree, built once per engine. Nothing under `Haiven/` or `host/` names
a version; `ci/engines.json` carries the matrix and `ci/run.py` selects a row.

Three things had to change, and each was measured on a second engine rather than
predicted:

**The descriptor's `EngineVersion` is a gate, not a minimum.** Unreal compares it
against the engine's own *compatible* version, so any fixed value excludes every
other engine — that is also why 5.8.2 needed `5.8.0` there. With `5.8.0` pinned,
UE 5.6.1 refuses the plugin outright: *"The 'Haiven' plugin was designed for build
5.8.0"*, and `Haiven.Plugin.Loads` fails while the three protocol tests still pass.
Omitting the field is what lets one descriptor load in both. `validate-plugin.mjs`
now requires it absent. A precompiled distribution is the exception: those
binaries genuinely are version-specific and stamp the field per version at
packaging time.

**The first thing that breaks is C#, not C++.** `BuildSettingsVersion.V7` and
`EngineIncludeOrderVersion.Unreal5_8` are enum members that older
UnrealBuildTool does not define, so 5.6 fails while *compiling the rules
assembly* — `error CS0117: 'BuildSettingsVersion' does not contain a definition
for 'V7'`, `Result: Failed (RulesError)` — before one translation unit is
compiled. `#if ENGINE_MINOR_VERSION` cannot help there; it is a different
language. `Latest` is defined in every supported version and is the right answer
for a plugin always built from source. The trade-off is real: `Latest` means
"this engine's newest", so the same source gets different default build settings
per engine, which is exactly what a from-source plugin wants and exactly what a
binary distribution cannot have. `validate-plugin.mjs` now fails on a pinned
`V<n>`/`Unreal5_<n>`.

**`EngineAssociation` cannot be version-agnostic**, since it is one value naming
one engine. It is empty in the tracked `HaivenTests.uproject` and the runner
stamps the selected engine's `MAJOR.MINOR` into its staged copy.

The plain-C++ protocol/session core needs no version guards at all. The runtime
glue deliberately uses APIs shared by the UE5 line (`UGameInstanceSubsystem`,
`FHttpModule`, `IWebSocket`, and `UBlueprintAsyncActionBase`) rather than naming
minor-version enums in source.

Measured on September 18, 2026: the current runtime-client plugin built and all
ten automation cases passed on **UE 5.8.2** (bundled Clang 20.1.8) and **UE
5.6.1** (bundled Clang 18.1.0). The new cases execute the session state machine
and verify the Blueprint nodes through reflection. They do not contact a live
HTTP/WebSocket fixture, so network-adapter execution remains a future engine
integration case. `#sdkunrealeditorci` still owns provisioned GitHub runners;
local engine proof does not establish hosted-runner availability.

## Releases

release-please (`.github/workflows/release-please.yml`) runs on `main`. After an
sdk-dev promotion merges, it opens a release pull request. That pull request
bumps `CHANGELOG.md`, `.release-please-manifest.json`, and `VersionName` in
`Haiven/Haiven.uplugin`. It does not change the integer `Version` field.
Merging the release pull request tags `v<version>` and publishes the GitHub
release. Do not bump `VersionName` by hand or push that tag yourself; a second
tag races the release pull request.

`.github/workflows/release.yml` runs on the `v*` tag. `ci/release.py` builds
every asset and refuses a tag that is not exactly `v` + `VersionName`. Each
build step also runs locally.

Assets, each a zip with one top-level `Haiven/` folder for `<Project>/Plugins/`:

- `haiven-unreal-plugin-source-<version>.zip`: the plugin without
  Binaries/Intermediate; builds against any engine in `ci/engines.json`.
- `haiven-unreal-plugin-<version>-UE<major.minor>-Linux.zip`: `RunUAT.sh
  BuildPlugin` output per engine, with prebuilt `Binaries/Linux` and the
  packaged descriptor's `EngineVersion` pinned to that engine.
- `SHA256SUMS` over the zips.

Win64 and Mac binaries are not built: they need a Windows or macOS runner with
the engine installed. Dispatching the workflow by hand with no tag is a dry run
that builds every asset at the branch's `VersionName` and publishes nothing.
The binary job runs on self-hosted runners, so the workflow must never gain a
`pull_request` trigger: this repository is public.
