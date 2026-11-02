#include "HaivenSubsystem.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "IWebSocket.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

	bool ParseServerFrame(const FString& Json, Haiven::FServerFrame& OutFrame)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid()) return false;

		FString Type;
		if (!Object->TryGetStringField(TEXT("type"), Type)) return false;
		OutFrame.Type = ToUtf8(Type);

		FString RequestId;
		if (Object->TryGetStringField(TEXT("requestId"), RequestId)) OutFrame.RequestId = ToUtf8(RequestId);

		bool bSuccess = false;
		if (Object->TryGetBoolField(TEXT("success"), bSuccess)) OutFrame.Success = bSuccess;

		FString Error;
		if (Object->TryGetStringField(TEXT("error"), Error)) OutFrame.Error = ToUtf8(Error);
		OutFrame.bHasPayload = Object->HasField(TEXT("payload"));
		return true;
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
	HostAccessToken = AccessToken;
	HostAccessTokenExpiry = ExpiresAtUtc;
}

FGuid UHaivenSubsystem::Request(
	const FString& Method,
	const FString& Path,
	const FString& BodyJson,
	FHaivenHttpComplete Completion)
{
	const FGuid Handle = FGuid::NewGuid();
	const FString Url = ResolveHttpUrl(Path);
	FString Token;
	if (Url.IsEmpty() || !GetAccessToken(Token))
	{
		FHaivenHttpResult Result;
		Result.Error = Url.IsEmpty()
			? TEXT("Haiven is not configured with an absolute base URL.")
			: TEXT("No unexpired Haiven access token is available.");
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
	HttpRequest->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
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
	HttpRequests.Add(Handle, MoveTemp(Pending));
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[WeakThis, Handle](FHttpRequestPtr, FHttpResponsePtr Response, bool bTransportSucceeded)
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

			DispatchToGameThread([WeakThis, Handle, Result]()
			{
				if (!WeakThis.IsValid()) return;
				FPendingHttp PendingRequest;
				if (!WeakThis->HttpRequests.RemoveAndCopyValue(Handle, PendingRequest)) return;
				PendingRequest.Completion.ExecuteIfBound(Result);
			});
		});

	if (!HttpRequest->ProcessRequest())
	{
		FPendingHttp Failed;
		HttpRequests.RemoveAndCopyValue(Handle, Failed);
		FHaivenHttpResult Result;
		Result.Error = TEXT("The Haiven HTTP request could not be started.");
		DispatchToGameThread([Completion = MoveTemp(Failed.Completion), Result]() mutable
		{
			Completion.ExecuteIfBound(Result);
		});
	}
	return Handle;
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
	DisconnectRealtime();
	if (Completion.IsBound()) ConnectCompletions.Add(MoveTemp(Completion));

	FString Token;
	if (!IsSecureRealtimeUrl(ClientOptions.RealtimeUrl) || !GetAccessToken(Token))
	{
		CompleteConnect(false, !IsSecureRealtimeUrl(ClientOptions.RealtimeUrl)
			? TEXT("Haiven requires a secure realtime URL (loopback ws:// is allowed for development).")
			: TEXT("No unexpired Haiven access token is available."));
		return;
	}

	const uint64 Generation = ++RealtimeGeneration;
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	RealtimeSession = MakeUnique<Haiven::FSessionCore>(
		[WeakThis](const Haiven::FClientFrame& Frame)
		{
			if (WeakThis.IsValid()) WeakThis->WriteClientFrame(Frame);
		},
		[WeakThis]()
		{
			if (!WeakThis.IsValid()) return std::string();
			FString CurrentToken;
			return WeakThis->GetAccessToken(CurrentToken) ? ToUtf8(CurrentToken) : std::string();
		},
		[]() { return ToUtf8(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)); });

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
		if (WeakThis.IsValid()) WeakThis->OnRealtimeFrame.Broadcast(FromUtf8(Frame.Type), FromUtf8(Json));
	};
	RealtimeSession->OnReply = [WeakThis](const Haiven::FServerFrame& Frame, const std::string& Json)
	{
		if (WeakThis.IsValid()) WeakThis->HandleRealtimeReply(Frame, Json);
	};
	RealtimeSession->OnEvent = [WeakThis](const Haiven::FServerFrame& Frame, const std::string& Json)
	{
		if (WeakThis.IsValid()) WeakThis->OnRealtimeEvent.Broadcast(FromUtf8(Frame.Type), FromUtf8(Json));
	};
	RealtimeSession->OnExpired = [WeakThis]()
	{
		if (WeakThis.IsValid()) WeakThis->OnAuthExpired.Broadcast();
	};
	RealtimeSession->OnClosed = [WeakThis](int Code, const std::string& Reason)
	{
		if (!WeakThis.IsValid()) return;
		const FString UnrealReason = FromUtf8(Reason);
		WeakThis->FailRealtimeRequests(UnrealReason.IsEmpty() ? TEXT("realtime socket closed") : UnrealReason);
		WeakThis->CompleteConnect(false, UnrealReason);
		WeakThis->OnRealtimeClosed.Broadcast(Code, UnrealReason);
	};

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

	SetRealtimeState(EHaivenRealtimeState::Connecting);
	RealtimeSocket->Connect();
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
	++RealtimeGeneration;
	TSharedPtr<IWebSocket> Socket = RealtimeSocket;
	RealtimeSocket.Reset();
	if (RealtimeSession)
	{
		RealtimeSession->Closed(1000, "client disconnect");
		RealtimeSession.Reset();
	}
	if (Socket.IsValid()) Socket->Close(1000, TEXT("client disconnect"));
	SetRealtimeState(EHaivenRealtimeState::Disconnected);
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
	OnRealtimeError.Broadcast(Error);
	if (RealtimeSession) RealtimeSession->Closed(1006, ToUtf8(Error));
	RealtimeSession.Reset();
	RealtimeSocket.Reset();
	SetRealtimeState(EHaivenRealtimeState::Disconnected);
}

void UHaivenSubsystem::HandleSocketClosed(uint64 Generation, int32 Code, const FString& Reason)
{
	if (Generation != RealtimeGeneration) return;
	if (RealtimeSession) RealtimeSession->Closed(Code, ToUtf8(Reason));
	RealtimeSession.Reset();
	RealtimeSocket.Reset();
	SetRealtimeState(EHaivenRealtimeState::Disconnected);
}

void UHaivenSubsystem::WriteClientFrame(const Haiven::FClientFrame& Frame)
{
	if (!RealtimeSocket.IsValid()) return;
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("type"), FromUtf8(Frame.Type));
	Object->SetStringField(TEXT("requestId"), FromUtf8(Frame.RequestId));
	if (Frame.PayloadJson.has_value())
	{
		if (Frame.Type == Haiven::AuthType)
		{
			Object->SetStringField(TEXT("payload"), FromUtf8(*Frame.PayloadJson));
		}
		else
		{
			TSharedPtr<FJsonValue> Payload;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FromUtf8(*Frame.PayloadJson));
			if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid()) return;
			Object->SetField(TEXT("payload"), Payload);
		}
	}

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (FJsonSerializer::Serialize(Object, Writer)) RealtimeSocket->Send(Json);
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
	TSharedPtr<FJsonValue> Payload;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadJson);
	if (FJsonSerializer::Deserialize(Reader, Payload) && Payload.IsValid()) return true;
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
