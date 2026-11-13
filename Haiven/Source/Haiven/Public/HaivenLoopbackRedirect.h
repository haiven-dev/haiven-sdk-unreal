// The desktop redirect for the game login handoff: a loopback HTTP address the
// game listens on while the system browser finishes sign-in (RFC 8252 section
// 7.3). These are the pure rules; UHaivenSubsystem::BeginLoopbackGameLogin owns
// the listener. Validation of the callback itself (state, configured query,
// one-use verifier) stays in Haiven::Handoff::FCore.

#pragma once

#include "CoreMinimal.h"

namespace Haiven::LoopbackRedirect
{
	/** Where a loopback redirect listens, split out of the registered redirect URL. */
	struct FEndpoint
	{
		/** The listening port, 1 to 65535. */
		uint32 Port = 0;
		/** The exact request path, for example /haiven/callback. */
		FString Path;
		/** scheme://authority/path with no query: the callback is rebuilt on it. */
		FString Base;
	};

	/**
	 * Accepts `http://127.0.0.1:<port>/<path>` or
	 * `http://localhost:<port>/<path>`, with an explicit port, a path other than
	 * "/" without a trailing slash, an optional query and no fragment. Anything
	 * else is not a redirect this game can receive itself, so it is refused before
	 * a transaction starts.
	 */
	HAIVEN_API bool Parse(const FString& RedirectUrl, FEndpoint& OutEndpoint, FString& OutError);

	/**
	 * Rebuilds the callback URL from the decoded query the engine's HTTP server
	 * hands over, percent-encoding each name and value again in a stable order.
	 * FHandoffCore then checks it exactly as it would a deep link.
	 */
	HAIVEN_API FString CallbackUrl(const FEndpoint& Endpoint, const TMap<FString, FString>& Query);

	/** True for an IPv4 127.0.0.0/8 peer, ::1, or an IPv4-mapped loopback peer. */
	HAIVEN_API bool IsLoopbackPeer(TConstArrayView<uint8> RawIp);

	/** The plain-text page the browser shows once the redirect reached the game. */
	HAIVEN_API FString BrowserPage();
}
