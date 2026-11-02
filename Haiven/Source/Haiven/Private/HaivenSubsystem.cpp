#include "HaivenSubsystem.h"

#include "Async/Async.h"
#include "HaivenHandoff.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformProcess.h"

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/rand.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
	bool SecureRandomBytes(TArray<uint8>& Out, int32 Count)
	{
		Out.SetNumUninitialized(Count);
		return RAND_bytes(Out.GetData(), Out.Num()) == 1;
	}
}

void UHaivenSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Handoff = MakeUnique<Haiven::FHandoffCore>();
}

void UHaivenSubsystem::Deinitialize()
{
	ShutdownNetworking();
	if (ActiveRequest.IsValid()) ActiveRequest->CancelRequest();
	ActiveRequest.Reset();
	Handoff.Reset();
	Super::Deinitialize();
}

bool UHaivenSubsystem::BeginGameLogin(const FHaivenGameLoginOptions& Options, FString& AuthorizationUrl, FString& Error)
{
	if (!Handoff) Handoff = MakeUnique<Haiven::FHandoffCore>();
	CancelGameLogin();

	TArray<uint8> StateEntropy;
	TArray<uint8> VerifierEntropy;
	if (!SecureRandomBytes(StateEntropy, 32) || !SecureRandomBytes(VerifierEntropy, 32))
	{
		Error = TEXT("The platform cryptographic random source failed.");
		return false;
	}

	Haiven::FHandoffConfig Config;
	Config.EntryUrl = Options.EntryUrl;
	Config.ExchangeUrl = Options.ExchangeUrl;
	Config.IntegrationId = Options.IntegrationId;
	Config.RedirectUrl = Options.RedirectUrl;
	Config.InstructionsId = Options.InstructionsId;
	Config.ScreenHint = Options.ScreenHint;
	Config.TransactionLifetime = FTimespan::FromSeconds(Options.TransactionLifetimeSeconds);
	FString State;
	if (!Handoff->Begin(Config, StateEntropy, VerifierEntropy, FDateTime::UtcNow(), AuthorizationUrl, State, Error)) return false;

	FString LaunchError;
	FPlatformProcess::LaunchURL(*AuthorizationUrl, nullptr, &LaunchError);
	if (!LaunchError.IsEmpty())
	{
		Handoff->Cancel();
		Error = TEXT("The platform URL launcher failed: ") + LaunchError;
		return false;
	}
	return true;
}

void UHaivenSubsystem::HandleGameLoginCallback(const FString& CallbackUrl)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, CallbackUrl]()
		{
			if (WeakThis.IsValid()) WeakThis->HandleGameLoginCallback(CallbackUrl);
		});
		return;
	}

	if (!Handoff)
	{
		BroadcastFailureOnGameThread(TEXT("The Haiven subsystem is not initialized."));
		return;
	}

	Haiven::FHandoffExchange Exchange;
	FString Error;
	if (!Handoff->ConsumeCallback(CallbackUrl, FDateTime::UtcNow(), Exchange, Error))
	{
		BroadcastFailureOnGameThread(Error);
		return;
	}

	ActiveRequest = FHttpModule::Get().CreateRequest();
	ActiveRequest->SetURL(Exchange.ExchangeUrl);
	// ExchangeUrl belongs to the active configuration; ToFormBody deliberately
	// contains exactly the four contract fields and no client secret.
	ActiveRequest->SetVerb(TEXT("POST"));
	ActiveRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	ActiveRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
	FString FormBody = Exchange.ToFormBody();
	ActiveRequest->SetContentAsString(FormBody);
	FMemory::Memzero(FormBody.GetCharArray().GetData(), FormBody.GetCharArray().Num() * sizeof(TCHAR));
	FormBody.Reset();
	FMemory::Memzero(Exchange.CodeVerifier.GetCharArray().GetData(), Exchange.CodeVerifier.GetCharArray().Num() * sizeof(TCHAR));
	Exchange.CodeVerifier.Reset();
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	ActiveRequest->OnProcessRequestComplete().BindLambda(
		[WeakThis](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (!WeakThis.IsValid() || WeakThis->ActiveRequest != Request) return;
			const int32 StatusCode = Response.IsValid() ? Response->GetResponseCode() : 0;
			const FString Body = Response.IsValid() ? Response->GetContentAsString() : FString();
			const FString TransportError = Response.IsValid() ? FString() : TEXT("no response");
			WeakThis->FinishExchange(bSucceeded, StatusCode, Body, TransportError);
		});
	if (!ActiveRequest->ProcessRequest())
	{
		ActiveRequest.Reset();
		BroadcastFailureOnGameThread(TEXT("The handoff exchange request could not be started."));
	}
}

void UHaivenSubsystem::CancelGameLogin()
{
	if (ActiveRequest.IsValid()) ActiveRequest->CancelRequest();
	ActiveRequest.Reset();
	if (Handoff) Handoff->Cancel();
}

void UHaivenSubsystem::Logout()
{
	CancelGameLogin();
	DisconnectRealtime();
	HostAccessToken.Reset();
	HostAccessTokenExpiry = FDateTime();
	if (Handoff) Handoff->ClearCredential();
}

bool UHaivenSubsystem::GetAccessToken(FString& AccessToken) const
{
	if (!HostAccessToken.IsEmpty() && HostAccessTokenExpiry > FDateTime::UtcNow())
	{
		AccessToken = HostAccessToken;
		return true;
	}
	return Handoff && Handoff->GetAccessToken(FDateTime::UtcNow(), AccessToken);
}

void UHaivenSubsystem::FinishExchange(bool bTransportSucceeded, int32 StatusCode, const FString& Body, const FString& TransportError)
{
	ActiveRequest.Reset();
	if (!bTransportSucceeded || StatusCode < 200 || StatusCode >= 300)
	{
		BroadcastFailureOnGameThread(FString::Printf(TEXT("Handoff exchange failed with HTTP %d: %s"), StatusCode, *TransportError));
		return;
	}

	Haiven::FHandoffCredential Credential;
	FString Error;
	if (!Handoff || !Handoff->StoreExchangeResponse(Body, FDateTime::UtcNow(), Credential, Error))
	{
		BroadcastFailureOnGameThread(Error);
		return;
	}

	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Credential]()
	{
		if (!WeakThis.IsValid()) return;
		FHaivenGameCredential BlueprintCredential;
		BlueprintCredential.AccessToken = Credential.AccessToken;
		BlueprintCredential.TokenType = Credential.TokenType;
		BlueprintCredential.ExpiresAtUtc = Credential.ExpiresAt;
		WeakThis->OnGameLoginCompleted.Broadcast(BlueprintCredential);
	});
}

void UHaivenSubsystem::BroadcastFailureOnGameThread(const FString& Error)
{
	TWeakObjectPtr<UHaivenSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Error]()
	{
		if (WeakThis.IsValid()) WeakThis->OnGameLoginFailed.Broadcast(Error);
	});
}
