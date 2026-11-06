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

	/**
	 * Reconnect realtime after the socket closes or fails unexpectedly, and
	 * authenticate the new socket with the current access token. Delays grow
	 * exponentially from RealtimeReconnectInitialDelaySeconds up to
	 * RealtimeReconnectMaxDelaySeconds, with jitter. DisconnectRealtime, Logout
	 * and an account change never reconnect, and neither does a socket the server
	 * refused for the account (a refused credential, a revoked game credential or
	 * a parental-consent lock).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Realtime")
	bool bReconnectRealtime = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Realtime", meta = (ClampMin = "0.1"))
	float RealtimeReconnectInitialDelaySeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|Realtime", meta = (ClampMin = "0.1"))
	float RealtimeReconnectMaxDelaySeconds = 30.0f;
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

/** One REST request, as a UHaivenRestRequests node builds it: send it with Send Rest Request. */
USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenRestRequest
{
	GENERATED_BODY()

	/** The HTTP method, upper case. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|REST")
	FString Method;

	/** The percent-encoded path and query, relative to the API base URL. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|REST")
	FString Path;

	/** The encoded body; empty when the operation sends none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|REST")
	FString Body;

	/** The body's media type; empty when the operation sends no body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|REST")
	FString ContentType;

	/** The success body's schema name (`X[]` for a list); empty when none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|REST")
	FString ResponseSchema;

	/** A redirect-only operation: open the base URL plus Path in a browser instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Haiven|REST")
	bool bNavigation = false;
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
