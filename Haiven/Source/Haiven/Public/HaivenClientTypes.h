#pragma once

#include "CoreMinimal.h"
#include "HaivenClientTypes.generated.h"

UENUM(BlueprintType)
enum class EHaivenRealtimeState : uint8
{
	Disconnected,
	Connecting,
	Authenticating,
	Ready,
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenClientOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven")
	FString BaseUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven")
	FString RealtimeUrl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven")
	FString AppId;
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenHttpResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|HTTP")
	bool bSucceeded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|HTTP")
	int32 StatusCode = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|HTTP")
	FString Body;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|HTTP")
	FString Error;
};

USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenRealtimeReply
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Realtime")
	bool bSucceeded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Realtime")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Realtime")
	FString RequestId;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Realtime")
	FString Json;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Realtime")
	FString Error;
};

DECLARE_DELEGATE_OneParam(FHaivenHttpComplete, const FHaivenHttpResult&);
DECLARE_DELEGATE_TwoParams(FHaivenConnectComplete, bool, const FString&);
DECLARE_DELEGATE_OneParam(FHaivenRealtimeComplete, const FHaivenRealtimeReply&);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenRealtimeStateChanged, EHaivenRealtimeState, State);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHaivenRealtimeFrameReceived, const FString&, Type, const FString&, Json);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHaivenRealtimeClosed, int32, Code, const FString&, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenRealtimeError, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FHaivenAuthExpired);
