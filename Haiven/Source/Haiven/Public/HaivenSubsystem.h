#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "HaivenClientTypes.h"
#include "HaivenHandoff.h"
#include "HaivenSessionCore.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HaivenSubsystem.generated.h"

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenGameLoginOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString EntryUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString ExchangeUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString IntegrationId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString RedirectUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString InstructionsId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth")
	FString ScreenHint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Auth", meta = (ClampMin = "1.0"))
	float TransactionLifetimeSeconds = 900.0f;
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenGameCredential
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	FString AccessToken;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	FString TokenType;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Auth")
	FDateTime ExpiresAtUtc;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenGameLoginCompleted, const FHaivenGameCredential&, Credential);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenGameLoginFailed, const FString&, Error);

/** Game-instance owner for Haiven's one-use browser login handoff. */
UCLASS(BlueprintType)
class HAIVEN_API UHaivenSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "Haiven")
	void Configure(const FHaivenClientOptions& InOptions);

	/** Supplies a host-owned bearer, including one restored from secure storage. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void SetAccessToken(const FString& AccessToken, FDateTime ExpiresAtUtc);

	/** Creates PKCE/state, stores the verifier transiently, and opens the system browser. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	bool BeginGameLogin(const FHaivenGameLoginOptions& Options, FString& AuthorizationUrl, FString& Error);

	/**
	 * Callback adapter for a platform deep-link handler. Validation happens before
	 * the form exchange; the verifier is consumed once even when the exchange fails.
	 */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void HandleGameLoginCallback(const FString& CallbackUrl);

	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void CancelGameLogin();

	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void Logout();

	UFUNCTION(BlueprintPure, Category = "Haiven|Auth")
	bool GetAccessToken(FString& AccessToken) const;

	/** Starts one authenticated REST request and returns its cancellation handle. */
	FGuid Request(
		const FString& Method,
		const FString& Path,
		const FString& BodyJson,
		FHaivenHttpComplete Completion);
	TFuture<FHaivenHttpResult> RequestAsync(
		const FString& Method,
		const FString& Path,
		const FString& BodyJson = FString());

	UFUNCTION(BlueprintCallable, Category = "Haiven|HTTP")
	void CancelHttpRequest(FGuid RequestHandle);

	/** Connects and completes only after first-frame authentication succeeds. */
	void ConnectRealtime(FHaivenConnectComplete Completion = FHaivenConnectComplete());
	TFuture<bool> ConnectRealtimeAsync();

	UFUNCTION(BlueprintCallable, Category = "Haiven|Realtime")
	void DisconnectRealtime();

	FString SendRealtimeRequest(
		const FString& Type,
		const FString& PayloadJson,
		FHaivenRealtimeComplete Completion);
	TFuture<FHaivenRealtimeReply> SendRealtimeRequestAsync(
		const FString& Type,
		const FString& PayloadJson = FString());

	UFUNCTION(BlueprintCallable, Category = "Haiven|Realtime")
	void CancelRealtimeRequest(const FString& RequestId);

	UFUNCTION(BlueprintPure, Category = "Haiven|Realtime")
	EHaivenRealtimeState GetRealtimeState() const { return RealtimeState; }

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Auth")
	FHaivenGameLoginCompleted OnGameLoginCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Auth")
	FHaivenGameLoginFailed OnGameLoginFailed;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeStateChanged OnRealtimeStateChanged;

	/** Every inbound frame, including auth frames consumed by the state machine. */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeFrameReceived OnRealtimeFrame;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeFrameReceived OnRealtimeEvent;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeClosed OnRealtimeClosed;

	UPROPERTY(BlueprintAssignable, Category = "Haiven|Realtime")
	FHaivenRealtimeError OnRealtimeError;

	/** Fires before same-socket re-auth reads the token owner again. */
	UPROPERTY(BlueprintAssignable, Category = "Haiven|Auth")
	FHaivenAuthExpired OnAuthExpired;

private:
	void FinishExchange(bool bTransportSucceeded, int32 StatusCode, const FString& Body, const FString& TransportError);
	void BroadcastFailureOnGameThread(const FString& Error);
	void ShutdownNetworking();
	void SetRealtimeState(EHaivenRealtimeState NewState);
	void HandleSocketConnected(uint64 Generation);
	void HandleSocketMessage(uint64 Generation, const FString& Json);
	void HandleSocketError(uint64 Generation, const FString& Error);
	void HandleSocketClosed(uint64 Generation, int32 Code, const FString& Reason);
	void WriteClientFrame(const Haiven::FClientFrame& Frame);
	void HandleRealtimeReply(const Haiven::FServerFrame& Frame, const std::string& Json);
	void CompleteConnect(bool bSucceeded, const FString& Error);
	void FailRealtimeRequests(const FString& Error);
	bool ValidatePayload(const FString& PayloadJson, FString& Error) const;
	FString ResolveHttpUrl(const FString& Path) const;

	struct FPendingHttp
	{
		TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> Request;
		FHaivenHttpComplete Completion;
	};

	TUniquePtr<Haiven::FHandoffCore> Handoff;
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
	FHaivenClientOptions ClientOptions;
	FString HostAccessToken;
	FDateTime HostAccessTokenExpiry;
	TMap<FGuid, FPendingHttp> HttpRequests;
	TSharedPtr<class IWebSocket> RealtimeSocket;
	TUniquePtr<Haiven::FSessionCore> RealtimeSession;
	TMap<FString, FHaivenRealtimeComplete> RealtimeRequests;
	TArray<FHaivenConnectComplete> ConnectCompletions;
	EHaivenRealtimeState RealtimeState = EHaivenRealtimeState::Disconnected;
	uint64 RealtimeGeneration = 0;
};
