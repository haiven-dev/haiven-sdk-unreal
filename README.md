# Haiven SDK for Unreal Engine

The Haiven client as an Unreal Engine plugin. It signs a player in with
Haiven's game-login handoff, calls the Haiven HTTP API, and keeps an
authenticated `/v0/ws` session.

The plugin is beta. `Haiven/Haiven.uplugin` reports `VersionName` `0.0.0`
and `IsBetaVersion` true. The behavior below matches this source tree.

## License

Haiven's software is Apache-2.0.

Copyright 2026 Haiven. The SPDX identifier is `Apache-2.0`.

Unreal Engine is not part of that license and is not in this repository.
The engine, its tools, and Epic's sample games are covered by the
[Unreal Engine EULA](https://www.unrealengine.com/eula/unreal). This
plugin is Haiven source that calls Unreal Engine APIs.

## Requirements

Tested on Linux with Unreal Engine **5.8.2** (primary) and **5.6.1**.
Other engine versions and platforms are outside that run. Prebuilt
binaries are Linux-only; Win64 and Mac need a machine with that engine
installed.

The game project must include C++. The `Haiven` module is a runtime
module and loads at `PreDefault`. It uses the engine `HTTP`, `Json`, and
`WebSockets` modules, and the engine's OpenSSL for PKCE. Leave the
`WebSocketNetworking` plugin disabled. That plugin is replication
transport. `Haiven.uplugin` names it only to keep it off.

`Haiven.uplugin` does not set `EngineVersion`. Unreal compares that field
with the engine's compatible version, not as a minimum, so a fixed value
loads in only one engine. The host targets under `host/Source/` set
`BuildSettingsVersion.Latest` and `EngineIncludeOrderVersion.Latest` for
the same reason: a pinned `V7` or `Unreal5_8` fails in UnrealBuildTool's
C# rules assembly on 5.6 before any C++ is compiled.

## Install

The plugin root is the `Haiven` directory that contains
`Haiven.uplugin`. The repository root is not itself a plugin. Place the
plugin at:

```text
<YourProject>/Plugins/Haiven/Haiven.uplugin
```

Copy or link `Haiven` into the game, regenerate project files, and
compile. Confirm **Haiven** is enabled under Edit → Plugins, and restart
the editor if it asks.

A release zip has one top-level `Haiven/` folder. Install that folder the
same way. Use a prebuilt zip only with the engine association encoded in
its file name. A prebuilt plugin stamps `EngineVersion` and will not load
in a different engine. The source zip does not stamp that field.

## Configure

`UHaivenSubsystem` is a `UGameInstanceSubsystem`. Call `Configure` before
any HTTP or realtime call.

| Field | Value |
| --- | --- |
| `BaseUrl` | `https` origin of the Haiven API. `http` is accepted only for `localhost`, `127.0.0.1`, or `::1`. A trailing slash is removed. |
| `RealtimeUrl` | `wss` URL of `/v0/ws`. `ws` is accepted only on those loopback hosts. |
| `AppId` | Optional. When set, HTTP requests send it as `X-Haiven-App-ID`. |

Use the host from the integration issued for the game.

## Game login

`BeginGameLogin` draws 32 bytes of state and 32 bytes of PKCE verifier
from the engine OpenSSL CSPRNG, keeps the verifier only in memory, opens
the authorization URL with `FPlatformProcess::LaunchURL`, and returns
that URL.

| Field | Meaning |
| --- | --- |
| `EntryUrl` | Absolute `https` URL of the game login page (`/connect/game` on the Haiven host). `http` is accepted only on loopback. |
| `ExchangeUrl` | Absolute `https` URL of `POST /v0/auth/handoff/exchange`, with the same loopback exception. |
| `IntegrationId` | Public integration id. There is no client secret. |
| `RedirectUrl` | Redirect URI registered for that integration. A custom scheme is allowed. |
| `InstructionsId`, `ScreenHint` | Optional query values on the authorization URL. |
| `TransactionLifetimeSeconds` | How long the verifier stays valid. The default is 900. |

Unreal does not expose one deep-link delegate for desktop and mobile.
When the platform delivers the redirect, pass the full callback URL to
`HandleGameLoginCallback`. The subsystem checks scheme, authority, path,
query, and state, consumes the verifier once, then posts exactly `code`,
`code_verifier`, `integration_id`, and `redirect_uri` as
`application/x-www-form-urlencoded`.

`OnGameLoginCompleted` and `OnGameLoginFailed` run on the game thread.
`CancelGameLogin` clears the verifier and cancels an exchange that has
not finished.

The stored credential is the access token, its token type, and an
absolute expiry, and it stays in memory. `GetAccessToken` returns false
after expiry. `Logout` clears it and disconnects realtime. A response
body that contains `refresh_token` is rejected. There is no refresh
call. When the token expires, start login again.

Do not write the bearer to a config file or to `USaveGame`. A game that
must keep it uses platform secure storage and restores it with
`SetAccessToken`.

```cpp
UHaivenSubsystem* Haiven = GetGameInstance()->GetSubsystem<UHaivenSubsystem>();

FHaivenClientOptions Client;
Client.BaseUrl = TEXT("https://<haiven-host>");
Client.RealtimeUrl = TEXT("wss://<haiven-host>/v0/ws");
Client.AppId = TEXT("<app-id>");
Haiven->Configure(Client);

Haiven->OnGameLoginCompleted.AddDynamic(
    this, &UYourGameSubsystem::OnHaivenLogin);
Haiven->OnGameLoginFailed.AddDynamic(
    this, &UYourGameSubsystem::OnHaivenLoginFailed);

FHaivenGameLoginOptions Login;
Login.EntryUrl = TEXT("https://<haiven-host>/connect/game");
Login.ExchangeUrl = TEXT("https://<haiven-host>/v0/auth/handoff/exchange");
Login.IntegrationId = TEXT("<public-integration-id>");
Login.RedirectUrl = TEXT("your-game://haiven/callback");

FString AuthorizationUrl;
FString Error;
if (!Haiven->BeginGameLogin(Login, AuthorizationUrl, Error))
{
    // Error is set. No browser was left on a partial login.
}
```

`OnHaivenLogin` and `OnHaivenLoginFailed` must be `UFUNCTION`s.
`BeginGameLogin` is also Blueprint-callable in the **Haiven|Auth**
category.

## REST and realtime

HTTP and realtime both require an unexpired access token, from login or
from `SetAccessToken`.

`Request` and `RequestAsync` send one call through `FHttpModule`. The
path is appended to `BaseUrl` (use a path such as `/v0/...`). The request
sends `Authorization: Bearer` and `X-Haiven-SDK`. Completion runs on the
game thread. `CancelHttpRequest` cancels by the returned `FGuid`.

`ConnectRealtime` opens an engine `IWebSocket` to `RealtimeUrl`. It
reports success only after `auth.success`. Until then the session queues
application frames. It correlates replies by request id, replays
unanswered frames with their original ids, and on expiry reads the token
from the subsystem again. `OnAuthExpired` fires before that read.
`DisconnectRealtime` closes the socket.

`SendRealtimeRequest` and `SendRealtimeRequestAsync` send one correlated
frame. `CancelRealtimeRequest` cancels by request id.

Blueprints have three async nodes for the same operations: **Http
Request**, **Connect Realtime**, and **Realtime Request**. Those nodes
are factories only. `Cancel` is a C++ method on the action object, and
the node does not return that object, so a Blueprint cannot call it.
`Request` and `SendRealtimeRequest` are not Blueprint-callable, so the
graph never receives the `FGuid` or request id that `CancelHttpRequest`
and `CancelRealtimeRequest` need. Close the socket from a Blueprint
with `DisconnectRealtime`.

`OnRealtimeFrame` receives every inbound frame, including auth frames.
`OnRealtimeEvent` receives server events only. Auth frames are applied to
session state and are not events or replies. A reply is a frame that
includes `success`. An event omits it. Absence is not `false`. Treating
a missing `success` as failure turns every event into a failed reply.

## Protocol core

`HaivenProtocol.h` and `HaivenSessionCore.h` are C++17 and include no
Unreal types, so `verify.sh` compiles them with an ordinary toolchain.
`success` is `std::optional<bool>`. A `static_assert` rejects narrowing
that field to `bool`. Values convert to Unreal types only at the engine
boundary. The subsystem, HTTP client, and WebSocket code are not part of
that engine-free build.

## Development

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
[host/README.md](host/README.md). Those editor tests do not open a live
HTTP or WebSocket fixture.

`conformance_test.cpp` drives `Haiven::FSessionCore` against an external
suite. It is not part of `verify.sh`. It prints a skip and exits 0 unless
`HAIVEN_CONFORMANCE_SUITE`, `HAIVEN_FIXTURE_BASE_URL`, and
`HAIVEN_FIXTURE_WS_URL` are set.

## Supporting more than one engine version

One source tree, built once per engine. Nothing under `Haiven/` or
`host/` names a version. `ci/engines.json` holds the matrix, and
`ci/run.py` selects a row.

Three constraints were measured on 5.8.2 and 5.6.1:

- **`EngineVersion` stays unset in source.** With `5.8.0` in the
  descriptor, UE 5.6.1 refuses the plugin ("The 'Haiven' plugin was
  designed for build 5.8.0"). Omitting the field is what lets one
  descriptor load in both. A prebuilt zip stamps the field for its own
  engine at pack time.
- **Target rules use `Latest`.** `BuildSettingsVersion.V7` and
  `EngineIncludeOrderVersion.Unreal5_8` are unknown to older
  UnrealBuildTool, so 5.6 fails while compiling the rules assembly
  (`CS0117`), before any translation unit is built. `Latest` means this
  engine's newest settings, which is what a from-source plugin wants.
- **`EngineAssociation` is per engine.** Tracked
  `host/HaivenTests.uproject` leaves it empty. `ci/run.py` writes the
  selected engine's major.minor into the staged copy.

The plain C++ core has no version guards. Runtime glue stays on APIs this
UE5 line shares: `UGameInstanceSubsystem`, `FHttpModule`, `IWebSocket`,
and `UBlueprintAsyncActionBase`.

Adding an engine means a new `ci/engines.json` row and a passing run of
that row. It does not widen the claim on its own.

## Releases

`.github/workflows/release.yml` publishes a GitHub release when a `v*`
tag is pushed. `ci/release.py` builds every asset. The same script runs
locally.

1. On `main`, set `VersionName` in `Haiven/Haiven.uplugin` to the release
   version and increment the integer `Version` by one. The workflow
   accepts a tag only when it is `v` plus that `VersionName`.
2. Tag that commit and push the tag:
   `git tag v1.2.3 && git push origin v1.2.3`.

Each asset is a zip whose only top-level entry is `Haiven/`, for
`<Project>/Plugins/`:

- `haiven-unreal-plugin-source-<version>.zip` — plugin source, without
  `Binaries` or `Intermediate`. It builds against the engines in
  `ci/engines.json`.
- `haiven-unreal-plugin-<version>-UE<major.minor>-Linux.zip` —
  `RunUAT.sh BuildPlugin` output for one engine, with prebuilt
  `Binaries/Linux` and `EngineVersion` pinned to that engine.
- `SHA256SUMS` covering those zips.

Win64 and Mac binaries are not built. Dispatching the workflow with no
tag is a dry run: it builds the assets at the branch's `VersionName` and
publishes nothing. The binary job runs on self-hosted runners. Do not add
a `pull_request` or `pull_request_target` trigger to that workflow. This
repository is public, and a pull-request trigger would run fork code on
those runners.

## Contributing

Bug reports, fixes, and documentation improvements are welcome. Open an
issue or a pull request against `main`:

<https://github.com/haiven-dev/haiven-sdk-unreal/issues>

Include the plugin `VersionName`, the Unreal Engine version, and the
steps that show the problem. Do not paste access tokens or private
endpoints. Commit subjects use Conventional Commits (`type(scope):
subject`). Run `./verify.sh` and `node validate-plugin.mjs`. When an
engine was available, name the version passed to `ci/run.py`.
