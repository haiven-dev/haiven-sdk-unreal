# Usage

Reference for the `Haiven` runtime module. The [README](../README.md) has
install steps and a minimal login example.

## Modules

The `Haiven` module is a runtime module and loads at `PreDefault`. It uses
the engine `HTTP`, `WebSockets`, `HTTPServer`, and `Sockets` modules (the
last two receive the loopback login redirect), and the engine's OpenSSL
for login entropy. The `HaivenEditor` module is editor-only and adds the
**Haiven Diagnostics** tab.

Leave the `WebSocketNetworking` plugin disabled. That plugin is
replication transport. `Haiven.uplugin` names it only to keep it off.

## Configure

`UHaivenSubsystem` is a `UGameInstanceSubsystem`. Call `Configure` before
any HTTP or realtime call.

| Field | Value |
| --- | --- |
| `BaseUrl` | `https` origin of the Haiven API. `http` is accepted only for `localhost`, `127.0.0.1`, or `::1`. A trailing slash is removed. |
| `RealtimeUrl` | `wss` URL of `/v0/ws`. `ws` is accepted only on those loopback hosts. |
| `AppId` | Optional. When set, HTTP requests send it as `X-Haiven-App-ID`. |
| `bReconnectRealtime` | Default true. Reconnects realtime after an unexpected close, with exponential backoff and jitter from `RealtimeReconnectInitialDelaySeconds` (1) to `RealtimeReconnectMaxDelaySeconds` (30). |

Use the host from the integration issued for the game.

## Game login

`BeginGameLogin` draws 32 bytes of state and 32 bytes of PKCE verifier
from the engine OpenSSL CSPRNG, keeps the verifier only in memory, opens
the authorization URL, and returns that URL. It opens the URL with
`FPlatformProcess::LaunchURL` unless `SetLoginUrlLauncher` installed a
different launcher.

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

On desktop, `BeginLoopbackGameLogin` needs no deep-link handler. It
listens on the `http://127.0.0.1:<port>/<path>` or
`http://localhost:<port>/<path>` redirect in `RedirectUrl`, which must be
registered exactly on the integration, and passes the browser's redirect
to the same callback checks. The route closes when the login ends.

`OnGameLoginCompleted` and `OnGameLoginFailed` run on the game thread.
`CancelGameLogin` clears the verifier and cancels an exchange that has
not finished.

### Credential

The credential is the access token, its token type, and an absolute
expiry, or no expiry for a durable credential. `GetAccessToken` returns
false after expiry. `Logout` clears it and disconnects realtime. A
response body that contains `refresh_token` is rejected.

Without a credential store the credential lasts until the game exits.
`SetCredentialStore` takes a game-supplied
`Haiven::IHandoffCredentialStore` (`Load`, `Store`, `Clear`), such as a
platform keychain. `ResumeGameLogin` then signs in from the saved
credential without opening the browser. Call it at start, before
`BeginGameLogin`. `Logout` and a refused credential clear the store.
Do not write the bearer to a config file or to `USaveGame`.

`SetAccessToken` replaces the signed-in account with a bearer the game
obtained itself. `RefreshAccessToken` swaps the bearer for the same
account without tearing down realtime.

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
`DisconnectRealtime` closes the socket and cancels a scheduled
reconnect.

`SendRealtimeRequest` and `SendRealtimeRequestAsync` send one correlated
frame. `CancelRealtimeRequest` cancels by request id.

`OnRealtimeFrame` receives every inbound frame, including auth frames.
`OnRealtimeEvent` receives server events only. Auth frames are applied to
session state and are not events or replies. A reply is a frame that
includes `success`. An event omits it. Absence is not `false`. Treating
a missing `success` as failure turns every event into a failed reply.

## Blueprints

`BeginGameLogin` is Blueprint-callable in the **Haiven|Auth** category.

Blueprints have async nodes for these operations: **Sign In With
Haiven**, **Http Request**, **Send Rest Request**, **Connect Realtime**,
and **Realtime Request**. Those nodes are factories only. `Cancel` is a
C++ method on the action object, and the node does not return that
object, so a Blueprint cannot call it. `Request` and
`SendRealtimeRequest` are not Blueprint-callable, so the graph never
receives the `FGuid` or request id that `CancelHttpRequest` and
`CancelRealtimeRequest` need. Close the socket from a Blueprint with
`DisconnectRealtime`, and abandon a login with `CancelGameLogin`.

## Other surfaces

`HaivenChat.h`, `HaivenRewards.h`, `HaivenAnalytics.h`,
`HaivenIdentity.h`, and `HaivenHealth.h` add chat, reward claims,
analytics, sign-in identity, and connection health. Generated Blueprint
types and REST operations are under
`Haiven/Source/Haiven/Public/Blueprint/`. These docs do not cover those
surfaces yet. Their headers do.

## Protocol core

`HaivenProtocol.h` and `HaivenSessionCore.h`, under
`Haiven/Source/ThirdParty/HaivenCpp/include/`, are C++17 and include no
Unreal types, so `verify.sh` compiles them with an ordinary toolchain.
`success` is `std::optional<bool>`. A `static_assert` rejects narrowing
that field to `bool`. Values convert to Unreal types only at the engine
boundary. The subsystem, HTTP client, and WebSocket code are not part of
that engine-free build.
