#pragma once

#include "CoreMinimal.h"
#include "HaivenChatCore.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "HaivenChat.generated.h"

class UHaivenSubsystem;

/** Where a message this player sent stands (Haiven::EOutgoingChatState). */
UENUM(BlueprintType)
enum class EHaivenOutgoingChatState : uint8
{
	/** Written, or queued until the session authenticates; no reply yet. */
	Sending,
	/** The socket closed before the reply. The next connect resends it under the same client message id. */
	Unconfirmed,
	/** Accepted and visible to the channel (`allowed` or `warned`). */
	Accepted,
	/** Accepted but held for moderation (`pending`, `review_pending`, or a state this build does not know). */
	Held,
	/** Accepted but not shown (`hidden` or `blocked`). */
	Removed,
	/** Refused with an error code; resending the same message cannot succeed. */
	Refused,
};

/** One message this player sent, tracked by its client message id (Haiven::FOutgoingChatMessage). */
USTRUCT(BlueprintType)
struct HAIVEN_API FHaivenOutgoingChatMessage
{
	GENERATED_BODY()

	/** The deduplication key: a resend under it is the same message to the server. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString ClientMessageId;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString ChannelId;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString Content;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	bool bHasParentMessageId = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString ParentMessageId;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	EHaivenOutgoingChatState State = EHaivenOutgoingChatState::Sending;

	/** The server's message id, once a reply accepted the message. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	bool bHasMessageId = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString MessageId;

	/** The reply's moderation state, kept verbatim (an open vocabulary). */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	bool bHasModerationState = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString ModerationState;

	/** The refusal's error code, when State is Refused. */
	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	bool bHasError = false;

	UPROPERTY(BlueprintReadOnly, Category = "Haiven|Chat")
	FString Error;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenOutgoingChatChanged, const FHaivenOutgoingChatMessage&, Message);

/**
 * The engine-side half of the plugin's chat: owns the C++ SDK's
 * Haiven::FChatCore, decodes its chat.send replies with the generated contract
 * codec, and presents its outbox in Blueprint types. UHaivenSubsystem owns one
 * and routes every realtime session into it; it is public so a game (and the
 * automation tests) can drive one over its own Haiven::FSessionCore.
 *
 * Like the core it outlives sessions: Attach each session, and route that
 * session's replies and its close here. Not thread-safe; use it on one thread
 * (the subsystem uses the game thread).
 */
class HAIVEN_API FHaivenChat
{
public:
	/** Client message ids are fresh lowercase GUIDs. */
	FHaivenChat();
	explicit FHaivenChat(Haiven::FChatCore::FClientMessageIdSource NewClientMessageId);

	FHaivenChat(const FHaivenChat&) = delete;
	FHaivenChat& operator=(const FHaivenChat&) = delete;

	/** Raised whenever an outgoing message changes state. */
	TFunction<void(const FHaivenOutgoingChatMessage&)> OnChanged;

	/** Uses this session from now on, and resends every unconfirmed message through it. */
	void Attach(Haiven::FSessionCore& Session);

	/**
	 * A reply from the attached session. A successful chat.send reply is decoded
	 * as ChatMessageSubmissionV0; one that does not decode leaves the message
	 * Held without a message id, never visible. Any other reply is ignored.
	 */
	void HandleReply(const Haiven::FServerFrame& Frame, const std::string& Json);

	/** The attached session's socket closed: unanswered sends become unconfirmed. */
	void HandleClosed();

	/**
	 * Sends a message, or keeps it to resend on the next attach. Fails, sending
	 * nothing, only when ChannelId is empty.
	 */
	bool Send(const FString& ChannelId, const FString& Content, const TOptional<FString>& ParentMessageId, FHaivenOutgoingChatMessage& OutMessage, FString& OutError);

	/** Messages this player sent, oldest first, until Forget. */
	TArray<FHaivenOutgoingChatMessage> Outbox() const;

	/** Stops tracking a message, normally one in a final state. */
	void Forget(const FString& ClientMessageId);

	static FHaivenOutgoingChatMessage ToBlueprint(const Haiven::FOutgoingChatMessage& Message);
	static EHaivenOutgoingChatState ToBlueprint(Haiven::EOutgoingChatState State);

private:
	Haiven::FChatCore Core;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenSendChatMessageOutput, const FHaivenOutgoingChatMessage&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHaivenSendChatMessageFailed, const FString&, Error);

/**
 * Sends one chat message through the subsystem's chat and waits for its final
 * state, across reconnects: a send cut off by a closed socket is resent under
 * the same client message id when the game connects again, and this node keeps
 * waiting. The message stays in the subsystem's outbox; call Forget Chat Message
 * when the game no longer shows it.
 */
UCLASS()
class HAIVEN_API UHaivenSendChatMessageAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AdvancedDisplay = "bHasParentMessageId,ParentMessageId"), Category = "Haiven|Chat")
	static UHaivenSendChatMessageAsyncAction* SendChatMessage(
		UObject* WorldContextObject,
		UHaivenSubsystem* Subsystem,
		const FString& ChannelId,
		const FString& Content,
		bool bHasParentMessageId,
		const FString& ParentMessageId);

	virtual void Activate() override;

	/** Stops waiting. The message itself is not withdrawn: it may still be delivered. */
	UFUNCTION(BlueprintCallable, Category = "Haiven|Chat")
	void Cancel();

	/** The message was queued or written; it carries its client message id. */
	UPROPERTY(BlueprintAssignable)
	FHaivenSendChatMessageOutput Sent;

	/** The server accepted it: State is Accepted, Held, or Removed. */
	UPROPERTY(BlueprintAssignable)
	FHaivenSendChatMessageOutput Settled;

	/** The server refused it with an error code (Message.Error). */
	UPROPERTY(BlueprintAssignable)
	FHaivenSendChatMessageOutput Refused;

	/** Nothing was sent: no subsystem, or an empty channel id. */
	UPROPERTY(BlueprintAssignable)
	FHaivenSendChatMessageFailed Failed;

private:
	UFUNCTION()
	void HandleChanged(const FHaivenOutgoingChatMessage& Message);

	void Finish();

	UPROPERTY()
	TObjectPtr<UHaivenSubsystem> Owner;
	FString Channel;
	FString Text;
	TOptional<FString> Parent;
	FString ClientMessageId;
};
