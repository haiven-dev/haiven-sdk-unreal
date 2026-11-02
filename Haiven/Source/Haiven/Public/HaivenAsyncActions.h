#pragma once

#include "CoreMinimal.h"
#include "HaivenClientTypes.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "HaivenAsyncActions.generated.h"

class UHaivenSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenHttpAsyncOutput, const FHaivenHttpResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FHaivenConnectAsyncSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenConnectAsyncFailure, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenRealtimeAsyncOutput, const FHaivenRealtimeReply&, Reply);

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
