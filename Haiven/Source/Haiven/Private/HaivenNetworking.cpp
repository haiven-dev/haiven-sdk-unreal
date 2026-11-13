#include "HaivenSubsystem.h"

#include "Async/Async.h"
#include "HaivenFrames.h"
#include "HttpModule.h"
#include "IWebSocket.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "WebSocketsModule.h"

namespace
{
	template <typename F>
	void DispatchToGameThread(F&& Function)
	{
		if (IsInGameThread())
		{
			Function();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, Forward<F>(Function));
		}
	}

	std::string ToUtf8(const FString& Value)
	{
		FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	FString FromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	/// The C++ SDK's parser (`HaivenFrames.h`): the one the conformance runners
	/// prove, so this plugin and every C++ host read the wire the same way.
	bool ParseServerFrame(const FString& Json, Haiven::FServerFrame& OutFrame)
	{
		return Haiven::ParseServerFrame(ToUtf8(Json), OutFrame);
	}

	FHaivenRealtimeReply FailedRealtimeReply(const FString& Error)
	{
		FHaivenRealtimeReply Result;
		Result.Error = Error;
		return Result;
	}

	bool IsLoopbackUrl(const FString& Url, const FString& Scheme)
	{
		if (!Url.StartsWith(Scheme, ESearchCase::IgnoreCase)) return false;
		FString Authority = Url.Mid(Scheme.Len());
		int32 End = Authority.Len();
		for (const TCHAR Separator : {TEXT('/'), TEXT('?'), TEXT('#')})
		{
			const int32 Found = Authority.Find(FString::Chr(Separator));
			if (Found != INDEX_NONE) End = FMath::Min(End, Found);
		}
		Authority.LeftInline(End);
		if (Authority.Contains(TEXT("@"))) return false;

		auto MatchesHost = [&Authority](const FString& Host)
		{
			if (Authority.Equals(Host, ESearchCase::IgnoreCase)) return true;
			const FString Prefix = Host + TEXT(":");
			return Authority.StartsWith(Prefix, ESearchCase::IgnoreCase)
				&& Authority.Mid(Prefix.Len()).IsNumeric();
		};
		return MatchesHost(TEXT("localhost"))
			|| MatchesHost(TEXT("127.0.0.1"))
			|| MatchesHost(TEXT("[::1]"));
	}

	bool IsSecureHttpBase(const FString& Url)
	{
		return Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)
			|| IsLoopbackUrl(Url, TEXT("http://"));
	}

	bool IsSecureRealtimeUrl(const FString& Url)
	{
		return Url.StartsWith(TEXT("wss://"), ESearchCase::IgnoreCase)
			|| IsLoopbackUrl(Url, TEXT("ws://"));
	}
}

void UHaivenSubsystem::Configure(const FHaivenClientOptions& InOptions)
{
	ClientOptions = InOptions;
	ClientOptions.BaseUrl.RemoveFromEnd(TEXT("/"));
}

void UHaivenSubsystem::SetAccessToken(const FString& AccessToken, FDateTime ExpiresAtUtc)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		DispatchToGameThread([WeakThis, AccessToken, ExpiresAtUtc]()
		{
			if (WeakThis.IsValid()) WeakThis->SetAccessToken(AccessToken, ExpiresAtUtc);
		});
		return;
	}
	// A supplied bearer replaces the prior account completely. Do not retain a
	// handoff credential which could become visible again after this one expires.
	if (Handoff) Handoff->ClearCredential();
	HostAccessToken = AccessToken;
	HostAccessTokenExpiry = ExpiresAtUtc;
	InvalidateCredentialBoundary();
	SubmitSavedRewardClaims();
}

void UHaivenSubsystem::RefreshAccessToken(const FString& AccessToken, FDateTime ExpiresAtUtc)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		DispatchToGameThread([WeakThis, AccessToken, ExpiresAtUtc]()
		{
			if (WeakThis.IsValid()) WeakThis->RefreshAccessToken(AccessToken, ExpiresAtUtc);
		});
		return;
	}
	// This is deliberately not an account boundary. In particular, an
	// OnAuthExpired listener runs inside FSessionCore::HandleExpired; destroying
	// RealtimeSession here would invalidate the stack frame which immediately
	// reads the token source and sends same-socket auth.
	if (Handoff) Handoff->ClearCredential();
	HostAccessToken = AccessToken;
	HostAccessTokenExpiry = ExpiresAtUtc;
}

FGuid UHaivenSubsystem::Request(
	const FString& Method,
	const FString& Path,
	const FString& BodyJson,
	FHaivenHttpComplete Completion)
{
	return RequestInternal(true, Method, Path, BodyJson, MoveTemp(Completion));
}

FGuid UHaivenSubsystem::RequestPublic(
	const FString& Method,
	const FString& Path,
	const FString& BodyJson,
	FHaivenHttpComplete Completion)
{
	return RequestInternal(false, Method, Path, BodyJson, MoveTemp(Completion));
}

FGuid UHaivenSubsystem::RequestInternal(
	bool bAuthenticated,
	const FString& Method,
	const FString& Path,
	const FString& BodyJson,
	FHaivenHttpComplete Completion)
{
	const FGuid Handle = FGuid::NewGuid();
	const FString Url = ResolveHttpUrl(Path);
	FString Token;
	if (Url.IsEmpty() || (bAuthenticated && !GetAccessToken(Token)))
	{
		FHaivenHttpResult Result;
		Result.Error = Url.IsEmpty()
			? TEXT("Haiven is not configured with an absolute base URL.")
			: TEXT("No unexpired Haiven access token is available.");
		RecordFailure(EHaivenHealthArea::Http, Result.Error);
		DispatchToGameThread([Completion = MoveTemp(Completion), Result]() mutable
		{
			Completion.ExecuteIfBound(Result);
		});
		return Handle;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);
	HttpRequest->SetVerb(Method.ToUpper());
	HttpRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
	if (bAuthenticated) HttpRequest->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
	HttpRequest->SetHeader(TEXT("X-Haiven-SDK"), TEXT("unreal/0.0.0"));
	if (!ClientOptions.AppId.IsEmpty()) HttpRequest->SetHeader(TEXT("X-Haiven-App-ID"), ClientOptions.AppId);
	if (!BodyJson.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpRequest->SetContentAsString(BodyJson);
	}

	FPendingHttp Pending;
	Pending.Request = HttpRequest;
	Pending.Completion = MoveTemp(Completion);
	Pending.CredentialGeneration = CredentialGeneration;
	Pending.bAuthenticated = bAuthenticated;
	const uint64 RequestCredentialGeneration = Pending.CredentialGeneration;
	HttpRequests.Add(Handle, MoveTemp(Pending));
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[WeakThis, Handle, RequestCredentialGeneration](FHttpRequestPtr, FHttpResponsePtr Response, bool bTransportSucceeded)
		{
			FHaivenHttpResult Result;
			Result.StatusCode = Response.IsValid() ? Response->GetResponseCode() : 0;
			Result.Body = Response.IsValid() ? Response->GetContentAsString() : FString();
			Result.bSucceeded = bTransportSucceeded && Result.StatusCode >= 200 && Result.StatusCode < 300;
			if (!Result.bSucceeded)
			{
				Result.Error = Response.IsValid()
					? FString::Printf(TEXT("Haiven HTTP request failed with status %d."), Result.StatusCode)
					: TEXT("Haiven HTTP request completed without a response.");
			}

			DispatchToGameThread([WeakThis, Handle, RequestCredentialGeneration, Result]()
			{
				if (!WeakThis.IsValid()) return;
				WeakThis->CompleteHttpRequest(Handle, RequestCredentialGeneration, Result);
			});
		});

	if (!HttpRequest->ProcessRequest())
	{
		FPendingHttp Failed;
		HttpRequests.RemoveAndCopyValue(Handle, Failed);
		FHaivenHttpResult Result;
		Result.Error = TEXT("The Haiven HTTP request could not be started.");
		RecordFailure(EHaivenHealthArea::Http, Result.Error);
		DispatchToGameThread([Completion = MoveTemp(Failed.Completion), Result]() mutable
		{
			Completion.ExecuteIfBound(Result);
		});
	}
	return Handle;
}

void UHaivenSubsystem::CompleteHttpRequest(
	FGuid Handle,
	uint64 RequestCredentialGeneration,
	const FHaivenHttpResult& Result)
{
	check(IsInGameThread());
	const FPendingHttp* Existing = HttpRequests.Find(Handle);
	if (!Existing) return;
	if (Existing->bAuthenticated
		&& (Existing->CredentialGeneration != RequestCredentialGeneration
			|| RequestCredentialGeneration != CredentialGeneration))
	{
		HttpRequests.Remove(Handle);
		return;
	}
	FPendingHttp PendingRequest;
	if (!HttpRequests.RemoveAndCopyValue(Handle, PendingRequest)) return;
	if (!Result.bSucceeded) RecordFailure(EHaivenHealthArea::Http, Result.Error);
	if (PendingRequest.bAuthenticated
		&& Haiven::FCredentialLifecycle::ForHttpStatus(Result.StatusCode) == Haiven::ECredentialAction::Clear)
	{
		ClearRejectedCredential();
		// Handoff credentials have no web refresh path. Hosts use the same finite
		// expiry signal as realtime to start a new handoff after the bearer is gone.
		OnAuthExpired.Broadcast();
	}
	PendingRequest.Completion.ExecuteIfBound(Result);
}

TArray<FHaivenHttpComplete> UHaivenSubsystem::InvalidateAuthenticatedHttpRequests()
{
	check(IsInGameThread());
	++CredentialGeneration;
	TArray<FHaivenHttpComplete> Completions;
	TArray<FGuid> Handles;
	HttpRequests.GenerateKeyArray(Handles);
	for (const FGuid& Handle : Handles)
	{
		const FPendingHttp* Existing = HttpRequests.Find(Handle);
		if (!Existing || !Existing->bAuthenticated) continue;
		FPendingHttp Stale;
		if (!HttpRequests.RemoveAndCopyValue(Handle, Stale)) continue;
		// Remove and unbind first: some transports complete synchronously from
		// CancelRequest. The transport result is stale; callers receive one finite
		// cancellation only after the whole account boundary has been reset.
		if (Stale.Request.IsValid())
		{
			Stale.Request->OnProcessRequestComplete().Unbind();
			Stale.Request->CancelRequest();
		}
		if (Stale.Completion.IsBound()) Completions.Add(MoveTemp(Stale.Completion));
	}
	return Completions;
}

TFuture<FHaivenHttpResult> UHaivenSubsystem::RequestAsync(
	const FString& Method,
	const FString& Path,
	const FString& BodyJson)
{
	TSharedRef<TPromise<FHaivenHttpResult>, ESPMode::ThreadSafe> Promise =
		MakeShared<TPromise<FHaivenHttpResult>, ESPMode::ThreadSafe>();
	TFuture<FHaivenHttpResult> Future = Promise->GetFuture();
	Request(Method, Path, BodyJson, FHaivenHttpComplete::CreateLambda(
		[Promise](const FHaivenHttpResult& Result) { Promise->SetValue(Result); }));
	return Future;
}

void UHaivenSubsystem::CancelHttpRequest(FGuid RequestHandle)
{
	FPendingHttp Pending;
	if (!HttpRequests.RemoveAndCopyValue(RequestHandle, Pending)) return;
	if (Pending.Request.IsValid()) Pending.Request->CancelRequest();
	FHaivenHttpResult Result;
	Result.Error = TEXT("cancelled");
	DispatchToGameThread([Completion = MoveTemp(Pending.Completion), Result]() mutable
	{
		Completion.ExecuteIfBound(Result);
	});
}

void UHaivenSubsystem::ConnectRealtime(FHaivenConnectComplete Completion)
{
	// The game's own connect supersedes a scheduled reconnect.
	CancelRealtimeReconnect();
	if (bRealtimeTeardown)
	{
		bRealtimeConnectDeferred = true;
		if (Completion.IsBound()) DeferredConnectCompletions.Add(MoveTemp(Completion));
		if (!bRealtimeConnectScheduled)
		{
			bRealtimeConnectScheduled = true;
			TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
			AsyncTask(ENamedThreads::GameThread, [WeakThis]
			{
				if (WeakThis.IsValid()) WeakThis->RunDeferredRealtimeConnect();
			});
		}
		return;
	}
	DisconnectRealtime();
	if (Completion.IsBound()) ConnectCompletions.Add(MoveTemp(Completion));

	FString Token;
	if (!IsSecureRealtimeUrl(ClientOptions.RealtimeUrl) || !GetAccessToken(Token))
	{
		const FString Error = !IsSecureRealtimeUrl(ClientOptions.RealtimeUrl)
			? TEXT("Haiven requires a secure realtime URL (loopback ws:// is allowed for development).")
			: TEXT("No unexpired Haiven access token is available.");
		RecordFailure(EHaivenHealthArea::Realtime, Error);
		CompleteConnect(false, Error);
		return;
	}

	bRealtimeAuthRefused = false;
	const uint64 Generation = ++RealtimeGeneration;
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	RealtimeSession = MakeUnique<Haiven::FSessionCore>(
		[WeakThis, Generation](const Haiven::FClientFrame& Frame)
		{
			// A retired session can still finish a callback it was running when it
			// was torn down (HandleExpired writes auth after OnExpired returns).
			// Its frames must never reach a socket a later connect created.
			if (WeakThis.IsValid() && WeakThis->RealtimeGeneration == Generation) WeakThis->WriteClientFrame(Frame);
		},
		[WeakThis]()
		{
			if (!WeakThis.IsValid()) return std::string();
			FString CurrentToken;
			return WeakThis->GetAccessToken(CurrentToken) ? ToUtf8(CurrentToken) : std::string();
		},
		[]() { return ToUtf8(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)); });

	BindRealtimeSession();

	RealtimeSocket = FWebSocketsModule::Get().CreateWebSocket(ClientOptions.RealtimeUrl);
	RealtimeSocket->OnConnected().AddLambda([WeakThis, Generation]()
	{
		DispatchToGameThread([WeakThis, Generation]()
		{
			if (WeakThis.IsValid()) WeakThis->HandleSocketConnected(Generation);
		});
	});
	RealtimeSocket->OnMessage().AddLambda([WeakThis, Generation](const FString& Json)
	{
		DispatchToGameThread([WeakThis, Generation, Json]()
		{
			if (WeakThis.IsValid()) WeakThis->HandleSocketMessage(Generation, Json);
		});
	});
	RealtimeSocket->OnConnectionError().AddLambda([WeakThis, Generation](const FString& Error)
	{
		DispatchToGameThread([WeakThis, Generation, Error]()
		{
			if (WeakThis.IsValid()) WeakThis->HandleSocketError(Generation, Error);
		});
	});
	RealtimeSocket->OnClosed().AddLambda([WeakThis, Generation](int32 Code, const FString& Reason, bool)
	{
		DispatchToGameThread([WeakThis, Generation, Code, Reason]()
		{
			if (WeakThis.IsValid()) WeakThis->HandleSocketClosed(Generation, Code, Reason);
		});
	});

	// The configured URL may carry gateway credentials in its query or userinfo.
	// Keep lifecycle diagnostics useful without copying that URL into support logs.
	UE_LOG(LogHaiven, Log, TEXT("Connecting realtime."));
	SetRealtimeState(EHaivenRealtimeState::Connecting);
	RealtimeSocket->Connect();
}

void UHaivenSubsystem::BindRealtimeSession()
{
	check(RealtimeSession);
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	// The generation this session was bound under. A callback that runs after a
	// later connect replaced the session must not change the replacement's state.
	const uint64 Generation = RealtimeGeneration;
	RealtimeSession->OnStateChanged = [WeakThis](Haiven::ESessionState State)
	{
		if (!WeakThis.IsValid()) return;
		switch (State)
		{
		case Haiven::ESessionState::Authenticating:
		case Haiven::ESessionState::Expired:
			WeakThis->SetRealtimeState(EHaivenRealtimeState::Authenticating);
			break;
		case Haiven::ESessionState::Ready:
			++WeakThis->RealtimeReadyCount;
			WeakThis->RealtimeReconnectAttempt = 0;
			WeakThis->SetRealtimeState(EHaivenRealtimeState::Ready);
			WeakThis->CompleteConnect(true, FString());
			break;
		case Haiven::ESessionState::Closed:
			WeakThis->SetRealtimeState(EHaivenRealtimeState::Disconnected);
			break;
		default:
			break;
		}
	};
	RealtimeSession->OnFrame = [WeakThis](const Haiven::FServerFrame& Frame, const std::string& Json)
	{
		if (!WeakThis.IsValid()) return;
		const FString Type = FromUtf8(Frame.Type);
		WeakThis->RecordRealtimeFrame(EHaivenRealtimeFrameDirection::Received, Type);
		WeakThis->OnRealtimeFrame.Broadcast(Type, FromUtf8(Json));
	};
	RealtimeSession->OnReply = [WeakThis](const Haiven::FServerFrame& Frame, const std::string& Json)
	{
		if (!WeakThis.IsValid()) return;
		// The chat's own sends are not in RealtimeRequests, and the chat ignores
		// every other request id, so each reply reaches exactly one owner.
		if (WeakThis->Chat) WeakThis->Chat->HandleReply(Frame, Json);
		WeakThis->HandleRealtimeReply(Frame, Json);
	};
	RealtimeSession->OnEvent = [WeakThis](const Haiven::FServerFrame& Frame, const std::string& Json)
	{
		DispatchToGameThread([WeakThis, Frame, Json]()
		{
			if (!WeakThis.IsValid()) return;
			// Domain state is applied before the raw event reaches gameplay. That also
			// invalidates any older REST snapshot so it cannot overwrite this event.
			WeakThis->HandleIdentityRealtimeEvent(Frame, Json);
			WeakThis->OnRealtimeEvent.Broadcast(FromUtf8(Frame.Type), FromUtf8(Json));
		});
	};
	RealtimeSession->OnExpired = [WeakThis]()
	{
		if (!WeakThis.IsValid()) return;
		UE_LOG(LogHaiven, Log, TEXT("Realtime credential expired; re-authenticating on the same socket."));
		WeakThis->OnAuthExpired.Broadcast();
	};
	RealtimeSession->OnAuthenticationFailed = [WeakThis, Generation](const std::string& Code)
	{
		// The same-socket re-auth was refused, so this socket will not recover.
		// The failure is always reported. For invalid_token and unauthorized the
		// session core has already raised RetryFreshSocket, whose handler below
		// replaced this session with a fresh one before this callback runs. That
		// fresh first-frame handshake decides: 1008 unauthorized clears the
		// credential, while 1013 auth_unavailable or a later ordinary drop keeps
		// it and reconnects. Only a refusal for the current session, with no
		// fresh-socket retry under way, stops automatic reconnects.
		if (!WeakThis.IsValid()) return;
		if (WeakThis->RealtimeGeneration == Generation) WeakThis->bRealtimeAuthRefused = true;
		const FString Error = TEXT("auth.failure: ") + FromUtf8(Code);
		WeakThis->RecordFailure(EHaivenHealthArea::Realtime, Error);
		WeakThis->OnRealtimeError.Broadcast(Error);
	};
	RealtimeSession->OnCredentialActionRequired = [WeakThis](Haiven::ECredentialAction Action)
	{
		if (!WeakThis.IsValid()) return;
		switch (Action)
		{
		case Haiven::ECredentialAction::Clear:
			WeakThis->ClearRejectedCredential();
			break;
		case Haiven::ECredentialAction::RetryFreshSocket:
			// A live auth.failure only rejects same-socket replacement. Retry the
			// same bearer as first-frame auth; that handshake can authoritatively
			// reject it with 1008 unauthorized.
			WeakThis->ConnectRealtime();
			break;
		default:
			break;
		}
	};
	RealtimeSession->OnClosed = [WeakThis](int Code, const std::string& Reason)
	{
		if (!WeakThis.IsValid()) return;
		const FString UnrealReason = FromUtf8(Reason);
		if (WeakThis->Chat) WeakThis->Chat->HandleClosed();
		WeakThis->FailRealtimeRequests(UnrealReason.IsEmpty() ? TEXT("realtime socket closed") : UnrealReason);
		WeakThis->CompleteConnect(false, UnrealReason);
		WeakThis->OnRealtimeClosed.Broadcast(Code, UnrealReason);
	};

	// Queued sends and resends of unconfirmed messages wait in the session until
	// first-frame authentication succeeds.
	if (Chat) Chat->Attach(*RealtimeSession);
}

TFuture<bool> UHaivenSubsystem::ConnectRealtimeAsync()
{
	TSharedRef<TPromise<bool>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<bool>, ESPMode::ThreadSafe>();
	TFuture<bool> Future = Promise->GetFuture();
	ConnectRealtime(FHaivenConnectComplete::CreateLambda(
		[Promise](bool bSucceeded, const FString&) { Promise->SetValue(bSucceeded); }));
	return Future;
}

void UHaivenSubsystem::DisconnectRealtime()
{
	// Deliberate, as are Logout and an account change, which close through here.
	CancelRealtimeReconnect();
	if (bRealtimeTeardown) return;
	{
		TGuardValue<bool> TeardownGuard(bRealtimeTeardown, true);
		++RealtimeGeneration;
		TUniquePtr<Haiven::FSessionCore> Session = MoveTemp(RealtimeSession);
		TSharedPtr<IWebSocket> Socket = MoveTemp(RealtimeSocket);
		// Clear the members before Closed invokes game callbacks. Any reconnect
		// requested by those callbacks is deferred until this scope fully unwinds.
		if (Session) Session->Closed(1000, "client disconnect");
		RetireRealtimeSession(MoveTemp(Session));
		if (Socket.IsValid()) Socket->Close(1000, TEXT("client disconnect"));
		SetRealtimeState(EHaivenRealtimeState::Disconnected);
	}
}

void UHaivenSubsystem::RetireRealtimeSession(TUniquePtr<Haiven::FSessionCore> Session)
{
	if (!Session) return;
	// Teardown can be requested from inside one of this session's own callbacks:
	// an OnAuthExpired listener that replaces the account with SetAccessToken, or
	// a Ready/frame listener that logs out or reconnects. FSessionCore is still on
	// the stack there (HandleExpired calls Authenticate() after OnExpired
	// returns), so it is released on a later game-thread turn, never in the frame
	// that closed it.
	RetiredRealtimeSessions.Add(MoveTemp(Session));
	if (bRealtimeRetirementScheduled) return;
	bRealtimeRetirementScheduled = true;
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis]
	{
		if (WeakThis.IsValid()) WeakThis->ReleaseRetiredRealtimeSessions();
	});
}

void UHaivenSubsystem::ReleaseRetiredRealtimeSessions()
{
	bRealtimeRetirementScheduled = false;
	TArray<TUniquePtr<Haiven::FSessionCore>> Released = MoveTemp(RetiredRealtimeSessions);
	RetiredRealtimeSessions.Reset();
}

void UHaivenSubsystem::RunDeferredRealtimeConnect()
{
	bRealtimeConnectScheduled = false;
	if (!bRealtimeConnectDeferred) return;
	if (bRealtimeTeardown)
	{
		bRealtimeConnectScheduled = true;
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis]
		{
			if (WeakThis.IsValid()) WeakThis->RunDeferredRealtimeConnect();
		});
		return;
	}
	bRealtimeConnectDeferred = false;
	TArray<FHaivenConnectComplete> Pending = MoveTemp(DeferredConnectCompletions);
	DeferredConnectCompletions.Reset();
	FHaivenConnectComplete Combined;
	if (!Pending.IsEmpty())
	{
		Combined = FHaivenConnectComplete::CreateLambda(
			[Pending = MoveTemp(Pending)](bool bSucceeded, const FString& Error) mutable
			{
				for (FHaivenConnectComplete& Completion : Pending)
					Completion.ExecuteIfBound(bSucceeded, Error);
			});
	}
	ConnectRealtime(MoveTemp(Combined));
}

void UHaivenSubsystem::CancelRealtimeReconnect()
{
	++RealtimeConnectIntent;
	if (RealtimeReconnectTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RealtimeReconnectTicker);
		RealtimeReconnectTicker.Reset();
	}
	// The scheduled reconnect itself connects through ConnectRealtime and keeps
	// its place in the backoff; anything the game asks for starts it over.
	if (!bRealtimeReconnecting) RealtimeReconnectAttempt = 0;
}

void UHaivenSubsystem::ScheduleRealtimeReconnect(uint64 Intent, int32 Code, const FString& Reason)
{
	// A close callback connected, disconnected, logged out or replaced the account.
	if (Intent != RealtimeConnectIntent) return;
	if (!ClientOptions.bReconnectRealtime) return;
	// The server refused this account: the credential (an auth.failure that asked
	// for no fresh-socket retry), a revoked game credential, a parental-consent
	// lock, or an unsupported protocol. A new socket would be refused the same way. A missed first-frame deadline is an
	// abnormal transport close, not a policy close, and reaches the retry path.
	if (bRealtimeAuthRefused) return;
	if (Code == 1008) return;
	FString Token;
	if (!GetAccessToken(Token)) return;

	const int32 Attempt = ++RealtimeReconnectAttempt;
	RealtimeReconnectDelay = RealtimeReconnectDelaySeconds(
		Attempt,
		ClientOptions.RealtimeReconnectInitialDelaySeconds,
		ClientOptions.RealtimeReconnectMaxDelaySeconds,
		FMath::FRand());
	UE_LOG(LogHaiven, Log, TEXT("Realtime closed unexpectedly; reconnecting in %.2f s (attempt %d)."),
		RealtimeReconnectDelay, Attempt);
	RealtimeReconnectTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(this, [this](float)
		{
			RealtimeReconnectTicker.Reset();
			RunRealtimeReconnect();
			return false;
		}),
		RealtimeReconnectDelay);
}

void UHaivenSubsystem::RunRealtimeReconnect()
{
	// A new session authenticates with whatever token is current now.
	TGuardValue<bool> Reconnecting(bRealtimeReconnecting, true);
	ConnectRealtime();
}

float UHaivenSubsystem::RealtimeReconnectDelaySeconds(int32 Attempt, float InitialSeconds, float MaxSeconds, float Jitter)
{
	const float Initial = FMath::Max(0.1f, InitialSeconds);
	const float Max = FMath::Max(Initial, MaxSeconds);
	float Step = Initial;
	for (int32 Index = 1; Index < Attempt && Step < Max; ++Index) Step *= 2.0f;
	Step = FMath::Min(Step, Max);
	// Equal jitter: never immediate, and clients dropped together spread out.
	return Step * (0.5f + 0.5f * FMath::Clamp(Jitter, 0.0f, 1.0f));
}

FString UHaivenSubsystem::SendRealtimeRequest(
	const FString& Type,
	const FString& PayloadJson,
	FHaivenRealtimeComplete Completion)
{
	FString Error;
	if (!RealtimeSession)
	{
		Error = TEXT("The Haiven realtime session is not connected.");
	}
	else if (Type.IsEmpty())
	{
		Error = TEXT("A realtime frame type is required.");
	}
	else if (!ValidatePayload(PayloadJson, Error))
	{
	}

	if (!Error.IsEmpty())
	{
		const FHaivenRealtimeReply Result = FailedRealtimeReply(Error);
		RecordFailure(EHaivenHealthArea::Realtime, Result.Error);
		DispatchToGameThread([Completion = MoveTemp(Completion), Result]() mutable
		{
			Completion.ExecuteIfBound(Result);
		});
		return FString();
	}

	try
	{
		const std::optional<std::string> Payload = PayloadJson.IsEmpty()
			? std::nullopt
			: std::optional<std::string>(ToUtf8(PayloadJson));
		const FString RequestId = FromUtf8(RealtimeSession->Send(ToUtf8(Type), Payload));
		if (Completion.IsBound()) RealtimeRequests.Add(RequestId, MoveTemp(Completion));
		return RequestId;
	}
	catch (const std::exception& Exception)
	{
		const FHaivenRealtimeReply Result = FailedRealtimeReply(UTF8_TO_TCHAR(Exception.what()));
		RecordFailure(EHaivenHealthArea::Realtime, Result.Error);
		DispatchToGameThread([Completion = MoveTemp(Completion), Result]() mutable
		{
			Completion.ExecuteIfBound(Result);
		});
		return FString();
	}
}

TFuture<FHaivenRealtimeReply> UHaivenSubsystem::SendRealtimeRequestAsync(
	const FString& Type,
	const FString& PayloadJson)
{
	TSharedRef<TPromise<FHaivenRealtimeReply>, ESPMode::ThreadSafe> Promise =
		MakeShared<TPromise<FHaivenRealtimeReply>, ESPMode::ThreadSafe>();
	TFuture<FHaivenRealtimeReply> Future = Promise->GetFuture();
	SendRealtimeRequest(Type, PayloadJson, FHaivenRealtimeComplete::CreateLambda(
		[Promise](const FHaivenRealtimeReply& Result) { Promise->SetValue(Result); }));
	return Future;
}

void UHaivenSubsystem::CancelRealtimeRequest(const FString& RequestId)
{
	FHaivenRealtimeComplete Completion;
	if (!RealtimeRequests.RemoveAndCopyValue(RequestId, Completion)) return;
	FHaivenRealtimeReply Result = FailedRealtimeReply(TEXT("cancelled"));
	Result.RequestId = RequestId;
	DispatchToGameThread([Completion = MoveTemp(Completion), Result]() mutable
	{
		Completion.ExecuteIfBound(Result);
	});
}

void UHaivenSubsystem::ShutdownNetworking()
{
	DisconnectRealtime();
	TArray<FGuid> Handles;
	HttpRequests.GenerateKeyArray(Handles);
	for (const FGuid& Handle : Handles) CancelHttpRequest(Handle);
}

void UHaivenSubsystem::SetRealtimeState(EHaivenRealtimeState NewState)
{
	if (RealtimeState == NewState) return;
	UE_LOG(LogHaiven, Log, TEXT("Realtime %s -> %s."),
		*UEnum::GetDisplayValueAsText(RealtimeState).ToString(),
		*UEnum::GetDisplayValueAsText(NewState).ToString());
	RealtimeState = NewState;
	OnRealtimeStateChanged.Broadcast(RealtimeState);
}

void UHaivenSubsystem::HandleSocketConnected(uint64 Generation)
{
	if (Generation != RealtimeGeneration || !RealtimeSession) return;
	RealtimeSession->Opened();
}

void UHaivenSubsystem::HandleSocketMessage(uint64 Generation, const FString& Json)
{
	if (Generation != RealtimeGeneration || !RealtimeSession) return;
	Haiven::FServerFrame Frame;
	if (!ParseServerFrame(Json, Frame))
	{
		HandleSocketError(Generation, TEXT("Haiven received an invalid realtime JSON envelope."));
		return;
	}
	RealtimeSession->Receive(Frame, ToUtf8(Json));
}

void UHaivenSubsystem::HandleSocketError(uint64 Generation, const FString& Error)
{
	if (Generation != RealtimeGeneration) return;
	if (bRealtimeTeardown) return;
	const uint64 Intent = RealtimeConnectIntent;
	{
		TGuardValue<bool> TeardownGuard(bRealtimeTeardown, true);
		++RealtimeGeneration;
		TUniquePtr<Haiven::FSessionCore> Session = MoveTemp(RealtimeSession);
		TSharedPtr<IWebSocket> Socket = MoveTemp(RealtimeSocket);
		++RealtimeDropCount;
		RecordFailure(EHaivenHealthArea::Realtime, Error);
		OnRealtimeError.Broadcast(Error);
		if (Session) Session->Closed(1006, ToUtf8(Error));
		RetireRealtimeSession(MoveTemp(Session));
		SetRealtimeState(EHaivenRealtimeState::Disconnected);
	}
	ScheduleRealtimeReconnect(Intent, 1006, Error);
}

void UHaivenSubsystem::HandleSocketClosed(uint64 Generation, int32 Code, const FString& Reason)
{
	if (Generation != RealtimeGeneration) return;
	if (bRealtimeTeardown) return;
	const uint64 Intent = RealtimeConnectIntent;
	{
		TGuardValue<bool> TeardownGuard(bRealtimeTeardown, true);
		++RealtimeGeneration;
		TUniquePtr<Haiven::FSessionCore> Session = MoveTemp(RealtimeSession);
		TSharedPtr<IWebSocket> Socket = MoveTemp(RealtimeSocket);
		++RealtimeDropCount;
		bHasLastClose = true;
		LastCloseCode = Code;
		LastCloseReason = Reason;
		UE_LOG(LogHaiven, Log, TEXT("Realtime socket closed by the server: %d %s"), Code, *Reason);
		if (Session) Session->Closed(Code, ToUtf8(Reason));
		RetireRealtimeSession(MoveTemp(Session));
		SetRealtimeState(EHaivenRealtimeState::Disconnected);
	}
	ScheduleRealtimeReconnect(Intent, Code, Reason);
}

void UHaivenSubsystem::WriteClientFrame(const Haiven::FClientFrame& Frame)
{
	if (!RealtimeSocket.IsValid()) return;
	// The C++ SDK's writer keeps a payload's text verbatim, so an id above 2^53
	// is not rounded through a double on the way out.
	std::string Json;
	if (Haiven::SerializeClientFrame(Frame, Json))
	{
		RealtimeSocket->Send(FromUtf8(Json));
		RecordRealtimeFrame(EHaivenRealtimeFrameDirection::Sent, FromUtf8(Frame.Type));
	}
}

void UHaivenSubsystem::HandleRealtimeReply(const Haiven::FServerFrame& Frame, const std::string& Json)
{
	if (!Frame.RequestId.has_value()) return;
	const FString RequestId = FromUtf8(*Frame.RequestId);
	FHaivenRealtimeComplete Completion;
	if (!RealtimeRequests.RemoveAndCopyValue(RequestId, Completion)) return;

	FHaivenRealtimeReply Result;
	Result.bSucceeded = Frame.Success.value_or(false);
	Result.Type = FromUtf8(Frame.Type);
	Result.RequestId = RequestId;
	Result.Json = FromUtf8(Json);
	if (Frame.Error.has_value()) Result.Error = FromUtf8(*Frame.Error);
	if (!Result.bSucceeded)
	{
		if (Result.Error.IsEmpty()) Result.Error = TEXT("Haiven realtime request failed.");
		RecordFailure(EHaivenHealthArea::Realtime, Result.Error);
	}
	Completion.ExecuteIfBound(Result);
}

void UHaivenSubsystem::CompleteConnect(bool bSucceeded, const FString& Error)
{
	TArray<FHaivenConnectComplete> Pending = MoveTemp(ConnectCompletions);
	ConnectCompletions.Reset();
	for (FHaivenConnectComplete& Completion : Pending) Completion.ExecuteIfBound(bSucceeded, Error);
}

void UHaivenSubsystem::FailRealtimeRequests(const FString& Error)
{
	TMap<FString, FHaivenRealtimeComplete> Pending = MoveTemp(RealtimeRequests);
	RealtimeRequests.Reset();
	for (TPair<FString, FHaivenRealtimeComplete>& Pair : Pending)
	{
		FHaivenRealtimeReply Result = FailedRealtimeReply(Error);
		Result.RequestId = Pair.Key;
		Pair.Value.ExecuteIfBound(Result);
	}
}

bool UHaivenSubsystem::ValidatePayload(const FString& PayloadJson, FString& Error) const
{
	if (PayloadJson.IsEmpty()) return true;
	Haiven::FJson Payload;
	std::string ParseError;
	if (Haiven::FJson::Parse(ToUtf8(PayloadJson), Payload, ParseError)) return true;
	Error = TEXT("Realtime payload must be valid JSON.");
	return false;
}

FString UHaivenSubsystem::ResolveHttpUrl(const FString& Path) const
{
	// Request paths never select an authority: otherwise an untrusted absolute
	// path could exfiltrate the bearer which this transport adds automatically.
	if (Path.Contains(TEXT("://")) || Path.StartsWith(TEXT("//"))) return FString();
	if (!IsSecureHttpBase(ClientOptions.BaseUrl)) return FString();
	return ClientOptions.BaseUrl + (Path.StartsWith(TEXT("/")) ? FString() : TEXT("/")) + Path;
}

bool UHaivenSubsystem::SendChatMessage(
	const FString& ChannelId,
	const FString& Content,
	bool bHasParentMessageId,
	const FString& ParentMessageId,
	FHaivenOutgoingChatMessage& Message,
	FString& Error)
{
	if (!Chat)
	{
		Error = TEXT("The Haiven subsystem is not initialized.");
		return false;
	}
	return Chat->Send(ChannelId, Content, bHasParentMessageId ? TOptional<FString>(ParentMessageId) : TOptional<FString>(), Message, Error);
}

TArray<FHaivenOutgoingChatMessage> UHaivenSubsystem::GetChatOutbox() const
{
	return Chat ? Chat->Outbox() : TArray<FHaivenOutgoingChatMessage>();
}

void UHaivenSubsystem::ForgetChatMessage(const FString& ClientMessageId)
{
	if (Chat) Chat->Forget(ClientMessageId);
}

void UHaivenSubsystem::ResetChat()
{
	Chat = MakeUnique<FHaivenChat>();
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	Chat->OnChanged = [WeakThis](const FHaivenOutgoingChatMessage& Message)
	{
		if (!WeakThis.IsValid()) return;
		UE_LOG(LogHaiven, Verbose, TEXT("Chat %s in %s is %s."), *Message.ClientMessageId, *Message.ChannelId,
			*UEnum::GetDisplayValueAsText(Message.State).ToString());
		WeakThis->OnOutgoingChatChanged.Broadcast(Message);
	};
	if (RealtimeSession) Chat->Attach(*RealtimeSession);
}
