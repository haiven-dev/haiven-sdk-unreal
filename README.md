# Haiven SDK for Unreal Engine

The Haiven client as an Unreal Engine plugin. It signs a player in with
Haiven's game-login handoff, calls the Haiven HTTP API, and keeps an
authenticated `/v0/ws` session.

The plugin is beta (`0.1.0-preview.1`).

## Requirements

- Unreal Engine **5.8.2** (primary) or **5.6.1**, tested on Linux.
  Prebuilt binaries are Linux-only; Win64 and Mac build from source on a
  machine with that engine installed. See
  [docs/engine-versions.md](docs/engine-versions.md).
- A game project that includes C++.
- The `WebSocketNetworking` plugin left disabled.

## Install

Place the `Haiven` directory (the one that contains `Haiven.uplugin`, not
the repository root) at:

```text
<YourProject>/Plugins/Haiven/Haiven.uplugin
```

Regenerate project files, compile, and confirm **Haiven** is enabled
under Edit → Plugins. A release zip has one top-level `Haiven/` folder;
install it the same way. Use a prebuilt zip only with the engine named in
its file name.

## Quick start

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

`OnHaivenLogin` and `OnHaivenLoginFailed` must be `UFUNCTION`s. Pass the
redirect to `HandleGameLoginCallback`, or use `BeginLoopbackGameLogin` on
desktop. After login, call the API with `Request` and open realtime with
`ConnectRealtime`.

## Documentation

- [Usage](docs/usage.md) — configuration, login, credentials, REST,
  realtime, Blueprints, and the engine-free protocol core.
- [Engine versions](docs/engine-versions.md) — how one source tree
  builds on several Unreal Engine versions.
- [Development](docs/development.md) — headless checks, engine
  automation, and conformance.
- [Releases](docs/releases.md) — tags, assets, and runners.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Issues:
<https://github.com/haiven-dev/haiven-sdk-unreal/issues>

## License

Apache-2.0. Copyright 2026 Haiven.

Unreal Engine is not part of that license and is not in this repository.
The engine, its tools, and Epic's sample games are covered by the
[Unreal Engine EULA](https://www.unrealengine.com/eula/unreal). This
plugin is Haiven source that calls Unreal Engine APIs.
