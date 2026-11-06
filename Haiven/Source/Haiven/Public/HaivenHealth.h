#pragma once

#include "CoreMinimal.h"
#include "HaivenChat.h"
#include "HaivenClientTypes.h"
#include "HaivenHealth.generated.h"

/**
 * The plugin's log category. Lifecycle lines (connect, ready, close) log at
 * Log, failures at Warning, per-message chat transitions at Verbose. No line
 * carries a credential, a request body, or chat content.
 */
HAIVEN_API DECLARE_LOG_CATEGORY_EXTERN(LogHaiven, Log, All);

/** Which part of the subsystem reported the most recent failure. */
UENUM(BlueprintType)
enum class EHaivenHealthArea : uint8
{
	None,
	Login,
	Http,
	Realtime,
};

/** Whether a redacted realtime frame was sent by or received from the gateway. */
UENUM(BlueprintType)
enum class EHaivenRealtimeFrameDirection : uint8
{
	Sent,
	Received,
};

/**
 * Metadata retained for editor diagnostics. Payloads, request ids, chat content,
 * and credentials are deliberately not retained.
 */
USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenRecentRealtimeFrame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	EHaivenRealtimeFrameDirection Direction = EHaivenRealtimeFrameDirection::Received;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	FDateTime SeenAtUtc;
};

/**
 * A point-in-time health snapshot of UHaivenSubsystem, for a game's status UI
 * or a support log. Reading it has no side effects.
 */
USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenHealth
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	EHaivenRealtimeState RealtimeState = EHaivenRealtimeState::Disconnected;

	/** An unexpired access token is available; the token itself is never exposed here. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	bool bHasAccessToken = false;

	/** Whether AccessTokenExpiresAtUtc describes the current credential. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	bool bHasAccessTokenExpiry = false;

	/**
	 * Current credential expiry, FDateTime::MaxValue() for a durable credential;
	 * the credential itself is never exposed here.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	FDateTime AccessTokenExpiresAtUtc;

	/** A browser login has begun and its callback has not completed or failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	bool bLoginInProgress = false;

	/** Times the realtime session reached Ready since the subsystem initialized. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 RealtimeReadyCount = 0;

	/** Times a realtime socket closed or failed after it was opened. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 RealtimeDropCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	bool bHasLastClose = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 LastCloseCode = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	FString LastCloseReason;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	EHaivenHealthArea LastErrorArea = EHaivenHealthArea::None;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	FString LastError;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	FDateTime LastErrorAtUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 PendingHttpRequests = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 PendingRealtimeRequests = 0;

	/** Chat outbox entries by state; see EHaivenOutgoingChatState. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 ChatSending = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 ChatUnconfirmed = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 ChatAccepted = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 ChatHeld = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 ChatRemoved = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	int32 ChatRefused = 0;

	/** Newest 32 sent/received known frame types and timestamps, without frame bodies. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Health")
	TArray<FHaivenRecentRealtimeFrame> RecentRealtimeFrames;

	/** Adds each outbox entry to the per-state chat counts. */
	void TallyChatOutbox(const TArray<FHaivenOutgoingChatMessage>& Outbox);
};
