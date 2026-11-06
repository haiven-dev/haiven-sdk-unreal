// One GET route on the engine's HTTP server for the duration of one loopback
// game login. Destroying the receiver unbinds the route.
#pragma once

#include "CoreMinimal.h"
#include "HaivenLoopbackRedirect.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;

class FHaivenLoopbackReceiver
{
public:
	/** Receives the rebuilt callback URL on the game thread; true when it belonged to the active transaction. */
	using FOnCallback = TFunction<bool(const FString& CallbackUrl)>;

	static TSharedPtr<FHaivenLoopbackReceiver> Start(
		const Haiven::LoopbackRedirect::FEndpoint& Endpoint,
		FOnCallback OnCallback,
		FString& OutError);

	~FHaivenLoopbackReceiver();

	const Haiven::LoopbackRedirect::FEndpoint& GetEndpoint() const { return Endpoint; }

private:
	FHaivenLoopbackReceiver() = default;

	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	Haiven::LoopbackRedirect::FEndpoint Endpoint;
	FOnCallback Callback;
	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle Route;
};
