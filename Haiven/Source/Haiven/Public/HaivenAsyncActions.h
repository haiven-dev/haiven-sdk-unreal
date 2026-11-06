#pragma once

#include "CoreMinimal.h"
#include "HaivenClientTypes.h"
#include "HaivenSubsystem.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "HaivenAsyncActions.generated.h"

class UHaivenSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenHttpAsyncOutput, const FHaivenHttpResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FHaivenConnectAsyncSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenConnectAsyncFailure, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenRealtimeAsyncOutput, const FHaivenRealtimeReply&, Reply);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenSignInAsyncStarted, const FString&, AuthorizationUrl);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenSignInAsyncSuccess, FDateTime, ExpiresAtUtc);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenSignInAsyncFailure, const FString&, Error);

UCLASS()
class HAIVEN_API UHaivenHttpRequestAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "Haiven|HTTP")
	static UHaivenHttpRequestAsyncAction* HttpRequest(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem,
		const FString& Method,
		const FString& Path,
		const FString& BodyJson);

	virtual void Activate() override;

	UFUNCTION(BlueprintCallable, Category = "Haiven|HTTP")
	void Cancel();

	UPROPERTY(BlueprintAssignable)
	FHaivenHttpAsyncOutput Completed;

	UPROPERTY(BlueprintAssignable)
	FHaivenHttpAsyncOutput Failed;

private:
	void Finish(const FHaivenHttpResult& Result);

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
	FString Verb;
	FString RequestPath;
	FString Body;
	FGuid Handle;
};

/** Sends a request built by a UHaivenRestRequests node. */
UCLASS()
class HAIVEN_API UHaivenRestRequestAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Sends Request through the subsystem. A navigation, or a body that is not JSON, fails without sending. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "Haiven|REST")
	static UHaivenRestRequestAsyncAction* SendRestRequest(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem,
		const FHaivenRestRequest& Request);

	virtual void Activate() override;

	UFUNCTION(BlueprintCallable, Category = "Haiven|REST")
	void Cancel();

	UPROPERTY(BlueprintAssignable)
	FHaivenHttpAsyncOutput Completed;

	UPROPERTY(BlueprintAssignable)
	FHaivenHttpAsyncOutput Failed;

	/** Why Request cannot be sent by this node, or empty when it can. */
	static FString Unsendable(const FHaivenRestRequest& Request);

private:
	void Finish(const FHaivenHttpResult& Result);

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
	FHaivenRestRequest Pending;
	FGuid Handle;
};

UCLASS()
class HAIVEN_API UHaivenConnectRealtimeAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "Haiven|Realtime")
	static UHaivenConnectRealtimeAsyncAction* ConnectRealtime(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem);

	virtual void Activate() override;

	UFUNCTION(BlueprintCallable, Category = "Haiven|Realtime")
	void Cancel();

	UPROPERTY(BlueprintAssignable)
	FHaivenConnectAsyncSuccess Connected;

	UPROPERTY(BlueprintAssignable)
	FHaivenConnectAsyncFailure Failed;

private:
	void Finish(bool bSucceeded, const FString& Error);

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
};

UCLASS()
class HAIVEN_API UHaivenRealtimeRequestAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "Haiven|Realtime")
	static UHaivenRealtimeRequestAsyncAction* RealtimeRequest(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem,
		const FString& Type,
		const FString& PayloadJson);

	virtual void Activate() override;

	UFUNCTION(BlueprintCallable, Category = "Haiven|Realtime")
	void Cancel();

	UPROPERTY(BlueprintAssignable)
	FHaivenRealtimeAsyncOutput Completed;

	UPROPERTY(BlueprintAssignable)
	FHaivenRealtimeAsyncOutput Failed;

private:
	void Finish(const FHaivenRealtimeReply& Reply);

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
	FString FrameType;
	FString Payload;
	FString RequestId;
};

/**
 * Sign In With Haiven: one game login from the button press to a usable bearer.
 * A loopback Redirect Url (`http://127.0.0.1:<port>/<path>`) is received by the
 * subsystem itself; any other registered redirect (a custom scheme or claimed
 * HTTPS link) still needs the host to pass the callback to Handle Game Login
 * Callback. Signed In carries only the expiry: the bearer stays in the
 * subsystem, which REST and realtime read it from.
 */
UCLASS()
class HAIVEN_API UHaivenSignInAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Sign In With Haiven"), Category = "Haiven|Auth")
	static UHaivenSignInAsyncAction* SignInWithHaiven(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem,
		const FHaivenGameLoginOptions& Options);

	virtual void Activate() override;

	/** Abandons the login; Failed fires with the cancellation. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Auth")
	void Cancel();

	/** The browser opened; the authorization URL carries no secret but is not needed by most games. */
	UPROPERTY(BlueprintAssignable)
	FHaivenSignInAsyncStarted BrowserOpened;

	UPROPERTY(BlueprintAssignable)
	FHaivenSignInAsyncSuccess SignedIn;

	UPROPERTY(BlueprintAssignable)
	FHaivenSignInAsyncFailure Failed;

private:
	UFUNCTION()
	void HandleCompleted(const FHaivenGameCredential& Credential);

	UFUNCTION()
	void HandleFailed(const FString& Error);

	void Finish();

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
	FHaivenGameLoginOptions LoginOptions;
	bool bFinished = false;
};
