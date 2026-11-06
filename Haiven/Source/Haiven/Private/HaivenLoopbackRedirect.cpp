// The loopback redirect's pure rules and the engine listener that receives it.
// The listener only moves the browser's request to the game thread and answers
// the browser; FHandoffCore decides whether the callback belongs to the active
// transaction.
#include "HaivenLoopbackRedirect.h"

#include "Async/Async.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HaivenLoopbackReceiver.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"

namespace Haiven::LoopbackRedirect
{
	bool Parse(const FString& RedirectUrl, FEndpoint& OutEndpoint, FString& OutError)
	{
		static const FString Scheme(TEXT("http://"));
		OutEndpoint = FEndpoint();
		if (!RedirectUrl.StartsWith(Scheme, ESearchCase::CaseSensitive) || RedirectUrl.Contains(TEXT("#")))
		{
			OutError = TEXT("A loopback redirect must be an http:// URL without a fragment.");
			return false;
		}
		const FString Rest = RedirectUrl.Mid(Scheme.Len());
		int32 PathStart = INDEX_NONE;
		if (!Rest.FindChar(TEXT('/'), PathStart))
		{
			OutError = TEXT("A loopback redirect needs a path, for example /haiven/callback.");
			return false;
		}
		int32 QueryStart = INDEX_NONE;
		if (Rest.FindChar(TEXT('?'), QueryStart) && QueryStart < PathStart)
		{
			OutError = TEXT("A loopback redirect needs a path, for example /haiven/callback.");
			return false;
		}
		const FString Authority = Rest.Left(PathStart);
		FString Host;
		FString PortText;
		if (!Authority.Split(TEXT(":"), &Host, &PortText) || (Host != TEXT("127.0.0.1") && Host != TEXT("localhost")))
		{
			OutError = TEXT("A loopback redirect must name 127.0.0.1 or localhost with an explicit port.");
			return false;
		}
		const bool bDigits = !PortText.IsEmpty() && PortText.Len() <= 5
			&& INDEX_NONE == PortText.FindLastCharByPredicate([](TCHAR C) { return C < TEXT('0') || C > TEXT('9'); });
		const int32 Port = bDigits ? FCString::Atoi(*PortText) : 0;
		if (Port < 1 || Port > 65535)
		{
			OutError = TEXT("A loopback redirect port must be between 1 and 65535.");
			return false;
		}
		const FString Path = QueryStart == INDEX_NONE ? Rest.Mid(PathStart) : Rest.Mid(PathStart, QueryStart - PathStart);
		// The engine's HTTP server normalizes away a trailing slash and cannot bind
		// "/", so either would make the rebuilt callback differ from the redirect.
		if (Path.EndsWith(TEXT("/")) || !FHttpPath(Path).IsValidPath())
		{
			OutError = TEXT("A loopback redirect path must not be \"/\", end with \"/\", or contain reserved characters.");
			return false;
		}
		OutEndpoint.Port = static_cast<uint32>(Port);
		OutEndpoint.Path = Path;
		OutEndpoint.Base = Scheme + Authority + Path;
		return true;
	}

	FString CallbackUrl(const FEndpoint& Endpoint, const TMap<FString, FString>& Query)
	{
		TArray<FString> Names;
		Query.GetKeys(Names);
		Names.Sort();
		FString Url = Endpoint.Base;
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			Url += Index == 0 ? TEXT("?") : TEXT("&");
			Url += FGenericPlatformHttp::UrlEncode(Names[Index]);
			Url += TEXT("=");
			Url += FGenericPlatformHttp::UrlEncode(Query.FindChecked(Names[Index]));
		}
		return Url;
	}

	bool IsLoopbackPeer(TConstArrayView<uint8> RawIp)
	{
		if (RawIp.Num() == 4) return RawIp[0] == 127;
		if (RawIp.Num() != 16) return false;
		bool bLeadingZero = true;
		for (int32 Index = 0; Index < 10; ++Index) bLeadingZero &= RawIp[Index] == 0;
		if (!bLeadingZero) return false;
		if (RawIp[10] == 0xFF && RawIp[11] == 0xFF) return RawIp[12] == 127;
		for (int32 Index = 10; Index < 15; ++Index)
		{
			if (RawIp[Index] != 0) return false;
		}
		return RawIp[15] == 1;
	}

	FString BrowserPage()
	{
		return TEXT("Haiven sign-in reached the game. You can close this tab and return to the game.");
	}
}

namespace
{
	TUniquePtr<FHttpServerResponse> Page(EHttpServerResponseCodes Code, const FString& Text)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Text, TEXT("text/plain; charset=utf-8"));
		Response->Code = Code;
		// The request URL carried a one-use code: never cache it or pass it on.
		Response->Headers.Add(TEXT("Cache-Control"), TArray<FString>{ TEXT("no-store") });
		Response->Headers.Add(TEXT("Referrer-Policy"), TArray<FString>{ TEXT("no-referrer") });
		return Response;
	}
}

TSharedPtr<FHaivenLoopbackReceiver> FHaivenLoopbackReceiver::Start(
	const Haiven::LoopbackRedirect::FEndpoint& Endpoint,
	FOnCallback OnCallback,
	FString& OutError)
{
	TSharedRef<FHaivenLoopbackReceiver> Receiver = MakeShareable(new FHaivenLoopbackReceiver());
	Receiver->Endpoint = Endpoint;
	Receiver->Callback = MoveTemp(OnCallback);

	FHttpServerModule& Server = FHttpServerModule::Get();
	// Listeners bind to the engine's DefaultBindAddress, "localhost" unless a
	// project overrides it; HandleRequest refuses non-loopback peers regardless.
	Server.StartAllListeners();
	Receiver->Router = Server.GetHttpRouter(Endpoint.Port, /*bFailOnBindFailure*/ true);
	if (!Receiver->Router.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not listen on loopback port %u; another program may be using it."), Endpoint.Port);
		return nullptr;
	}
	TWeakPtr<FHaivenLoopbackReceiver> Weak = Receiver;
	Receiver->Route = Receiver->Router->BindRoute(
		FHttpPath(Endpoint.Path),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([Weak](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const TSharedPtr<FHaivenLoopbackReceiver> Pinned = Weak.Pin();
			return Pinned.IsValid() && Pinned->HandleRequest(Request, OnComplete);
		}));
	if (!Receiver->Route.IsValid())
	{
		OutError = FString::Printf(TEXT("The loopback path %s on port %u is already in use in this process."), *Endpoint.Path, Endpoint.Port);
		return nullptr;
	}
	return Receiver;
}

FHaivenLoopbackReceiver::~FHaivenLoopbackReceiver()
{
	// The engine keeps the port's listener for other routes; only this route goes.
	if (Router.IsValid() && Route.IsValid()) Router->UnbindRoute(Route);
}

bool FHaivenLoopbackReceiver::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// The router matches the longest bound prefix and hands over the remainder
	// as RelativePath, which is "/" only for the bound path itself. Reading that
	// rather than FHttpServerRequest::FullPath keeps 5.6, which lacks FullPath.
	if (!Request.RelativePath.IsRoot()) return false;
	if (!Request.PeerAddress.IsValid() || !Haiven::LoopbackRedirect::IsLoopbackPeer(Request.PeerAddress->GetRawIp()))
	{
		OnComplete(Page(EHttpServerResponseCodes::Forbidden, TEXT("Haiven sign-in only accepts this device's own browser.")));
		return true;
	}
	// Run the callback on a later game-thread turn: it ends the transaction,
	// which unbinds this route, and the router is still executing it here.
	const FString CallbackUrl = Haiven::LoopbackRedirect::CallbackUrl(Endpoint, Request.QueryParams);
	FOnCallback Deliver = Callback;
	FHttpResultCallback Respond = OnComplete;
	AsyncTask(ENamedThreads::GameThread, [Deliver = MoveTemp(Deliver), Respond = MoveTemp(Respond), CallbackUrl]()
	{
		const bool bAccepted = Deliver && Deliver(CallbackUrl);
		Respond(bAccepted
			? Page(EHttpServerResponseCodes::Ok, Haiven::LoopbackRedirect::BrowserPage())
			: Page(EHttpServerResponseCodes::BadRequest, TEXT("This sign-in link does not belong to the game's current sign-in. Start again from the game.")));
	});
	return true;
}
