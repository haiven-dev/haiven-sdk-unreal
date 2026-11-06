#include "HaivenAsyncActions.h"

#include "HaivenLoopbackRedirect.h"

#include "HaivenSubsystem.h"

UHaivenHttpRequestAsyncAction* UHaivenHttpRequestAsyncAction::HttpRequest(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FString& Method,
	const FString& Path,
	const FString& BodyJson)
{
	UHaivenHttpRequestAsyncAction* Action = NewObject<UHaivenHttpRequestAsyncAction>();
	Action->Owner = Subsystem;
	Action->Verb = Method;
	Action->RequestPath = Path;
	Action->Body = BodyJson;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenHttpRequestAsyncAction::Activate()
{
	if (!Owner)
	{
		FHaivenHttpResult Result;
		Result.Error = TEXT("The Haiven subsystem is unavailable.");
		Finish(Result);
		return;
	}
	Handle = Owner->Request(Verb, RequestPath, Body, FHaivenHttpComplete::CreateUObject(this, &UHaivenHttpRequestAsyncAction::Finish));
}

void UHaivenHttpRequestAsyncAction::Cancel()
{
	if (Owner && Handle.IsValid()) Owner->CancelHttpRequest(Handle);
}

void UHaivenHttpRequestAsyncAction::Finish(const FHaivenHttpResult& Result)
{
	if (Result.bSucceeded) Completed.Broadcast(Result);
	else Failed.Broadcast(Result);
	SetReadyToDestroy();
}

UHaivenRestRequestAsyncAction* UHaivenRestRequestAsyncAction::SendRestRequest(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FHaivenRestRequest& Request)
{
	UHaivenRestRequestAsyncAction* Action = NewObject<UHaivenRestRequestAsyncAction>();
	Action->Owner = Subsystem;
	Action->Pending = Request;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

FString UHaivenRestRequestAsyncAction::Unsendable(const FHaivenRestRequest& Request)
{
	// The subsystem sends JSON bodies only; a navigation would lose its redirect.
	if (Request.bNavigation)
	{
		return TEXT("This operation is a browser navigation: open the base URL plus its Path instead of sending it.");
	}
	if (!Request.ContentType.IsEmpty() && Request.ContentType != TEXT("application/json"))
	{
		return FString::Printf(TEXT("This node sends JSON bodies only, not %s."), *Request.ContentType);
	}
	return FString();
}

void UHaivenRestRequestAsyncAction::Activate()
{
	FHaivenHttpResult Refused;
	Refused.Error = Owner ? Unsendable(Pending) : FString(TEXT("The Haiven subsystem is unavailable."));
	if (!Refused.Error.IsEmpty())
	{
		Finish(Refused);
		return;
	}
	Handle = Owner->Request(Pending.Method, Pending.Path, Pending.Body, FHaivenHttpComplete::CreateUObject(this, &UHaivenRestRequestAsyncAction::Finish));
}

void UHaivenRestRequestAsyncAction::Cancel()
{
	if (Owner && Handle.IsValid()) Owner->CancelHttpRequest(Handle);
}

void UHaivenRestRequestAsyncAction::Finish(const FHaivenHttpResult& Result)
{
	if (Result.bSucceeded) Completed.Broadcast(Result);
	else Failed.Broadcast(Result);
	SetReadyToDestroy();
}

UHaivenConnectRealtimeAsyncAction* UHaivenConnectRealtimeAsyncAction::ConnectRealtime(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem)
{
	UHaivenConnectRealtimeAsyncAction* Action = NewObject<UHaivenConnectRealtimeAsyncAction>();
	Action->Owner = Subsystem;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenConnectRealtimeAsyncAction::Activate()
{
	if (!Owner)
	{
		Finish(false, TEXT("The Haiven subsystem is unavailable."));
		return;
	}
	Owner->ConnectRealtime(FHaivenConnectComplete::CreateUObject(this, &UHaivenConnectRealtimeAsyncAction::Finish));
}

void UHaivenConnectRealtimeAsyncAction::Cancel()
{
	if (Owner) Owner->DisconnectRealtime();
}

void UHaivenConnectRealtimeAsyncAction::Finish(bool bSucceeded, const FString& Error)
{
	if (bSucceeded) Connected.Broadcast();
	else Failed.Broadcast(Error.IsEmpty() ? TEXT("The Haiven realtime connection closed before authentication.") : Error);
	SetReadyToDestroy();
}

UHaivenRealtimeRequestAsyncAction* UHaivenRealtimeRequestAsyncAction::RealtimeRequest(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FString& Type,
	const FString& PayloadJson)
{
	UHaivenRealtimeRequestAsyncAction* Action = NewObject<UHaivenRealtimeRequestAsyncAction>();
	Action->Owner = Subsystem;
	Action->FrameType = Type;
	Action->Payload = PayloadJson;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenRealtimeRequestAsyncAction::Activate()
{
	if (!Owner)
	{
		FHaivenRealtimeReply Reply;
		Reply.Error = TEXT("The Haiven subsystem is unavailable.");
		Finish(Reply);
		return;
	}
	RequestId = Owner->SendRealtimeRequest(
		FrameType,
		Payload,
		FHaivenRealtimeComplete::CreateUObject(this, &UHaivenRealtimeRequestAsyncAction::Finish));
}

void UHaivenRealtimeRequestAsyncAction::Cancel()
{
	if (Owner && !RequestId.IsEmpty()) Owner->CancelRealtimeRequest(RequestId);
}

void UHaivenRealtimeRequestAsyncAction::Finish(const FHaivenRealtimeReply& Reply)
{
	if (Reply.bSucceeded) Completed.Broadcast(Reply);
	else Failed.Broadcast(Reply);
	SetReadyToDestroy();
}

UHaivenSignInAsyncAction* UHaivenSignInAsyncAction::SignInWithHaiven(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FHaivenGameLoginOptions& Options)
{
	UHaivenSignInAsyncAction* Action = NewObject<UHaivenSignInAsyncAction>();
	Action->Owner = Subsystem;
	Action->LoginOptions = Options;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenSignInAsyncAction::Activate()
{
	if (!Owner)
	{
		HandleFailed(TEXT("The Haiven subsystem is unavailable."));
		return;
	}
	Owner->OnGameLoginCompleted.AddUniqueDynamic(this, &UHaivenSignInAsyncAction::HandleCompleted);
	Owner->OnGameLoginFailed.AddUniqueDynamic(this, &UHaivenSignInAsyncAction::HandleFailed);
	Haiven::LoopbackRedirect::FEndpoint Endpoint;
	FString Ignored;
	const bool bLoopback = Haiven::LoopbackRedirect::Parse(LoginOptions.RedirectUrl, Endpoint, Ignored);
	FString AuthorizationUrl;
	FString Error;
	const bool bStarted = bLoopback
		? Owner->BeginLoopbackGameLogin(LoginOptions, AuthorizationUrl, Error)
		: Owner->BeginGameLogin(LoginOptions, AuthorizationUrl, Error);
	if (!bStarted)
	{
		HandleFailed(Error);
		return;
	}
	BrowserOpened.Broadcast(AuthorizationUrl);
}

void UHaivenSignInAsyncAction::Cancel()
{
	if (bFinished) return;
	if (Owner) Owner->CancelGameLogin();
	HandleFailed(TEXT("Sign-in was cancelled. No account was connected."));
}

void UHaivenSignInAsyncAction::HandleCompleted(const FHaivenGameCredential& Credential)
{
	if (bFinished) return;
	const FDateTime ExpiresAtUtc = Credential.ExpiresAtUtc;
	Finish();
	SignedIn.Broadcast(ExpiresAtUtc);
}

void UHaivenSignInAsyncAction::HandleFailed(const FString& Error)
{
	// A deep link with another login's state fails without consuming this one.
	if (bFinished || (Owner && Owner->IsGameLoginInProgress())) return;
	Finish();
	Failed.Broadcast(Error);
}

void UHaivenSignInAsyncAction::Finish()
{
	bFinished = true;
	if (Owner)
	{
		Owner->OnGameLoginCompleted.RemoveAll(this);
		Owner->OnGameLoginFailed.RemoveAll(this);
	}
	SetReadyToDestroy();
}

