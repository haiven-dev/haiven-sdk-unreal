#include "HaivenChat.h"

#include "Contracts/HaivenContractsJson.h"
#include "HaivenSubsystem.h"

#include <optional>
#include <string>

// Named rather than anonymous: a unity build joins this file with the plugin's
// others, and an anonymous helper of the same name there would collide.
namespace HaivenChatDetail
{
	std::string ToUtf8(const FString& Value)
	{
		FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	FString FromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	void Assign(const std::optional<std::string>& Source, bool& bHas, FString& Target)
	{
		bHas = Source.has_value();
		Target = bHas ? FromUtf8(*Source) : FString();
	}

	bool IsFinal(EHaivenOutgoingChatState State)
	{
		return State == EHaivenOutgoingChatState::Accepted
			|| State == EHaivenOutgoingChatState::Held
			|| State == EHaivenOutgoingChatState::Removed
			|| State == EHaivenOutgoingChatState::Refused;
	}
}

FHaivenChat::FHaivenChat()
	: FHaivenChat([]() { return HaivenChatDetail::ToUtf8(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)); })
{
}

FHaivenChat::FHaivenChat(Haiven::FChatCore::FClientMessageIdSource NewClientMessageId)
	: Core(MoveTemp(NewClientMessageId))
{
	Core.OnOutgoingChanged = [this](const Haiven::FOutgoingChatMessage& Message)
	{
		if (OnChanged) OnChanged(ToBlueprint(Message));
	};
}

void FHaivenChat::Attach(Haiven::FSessionCore& Session)
{
	Core.Attach(Session);
}

void FHaivenChat::HandleReply(const Haiven::FServerFrame& Frame, const std::string& Json)
{
	if (Frame.Type != "chat.send") return;
	std::optional<std::string> ModerationState;
	std::optional<std::string> MessageId;
	if (Frame.Success.value_or(false))
	{
		Haiven::WsServerReplyV0 Reply;
		Haiven::ChatMessageSubmissionV0 Submission;
		std::string Problem;
		if (Haiven::ContractsJson::FromJson(Json, Reply, Problem) && Reply.Payload.has_value()
			&& Haiven::ContractsJson::FromJson(*Reply.Payload, Submission, Problem))
		{
			ModerationState = Submission.ModerationState;
			MessageId = Submission.Message.Id;
		}
	}
	Core.HandleReply(Frame, ModerationState, MessageId);
}

void FHaivenChat::HandleClosed()
{
	Core.HandleClosed();
}

bool FHaivenChat::Send(const FString& ChannelId, const FString& Content, const TOptional<FString>& ParentMessageId, FHaivenOutgoingChatMessage& OutMessage, FString& OutError)
{
	if (ChannelId.IsEmpty())
	{
		OutError = TEXT("A chat channel id is required.");
		return false;
	}
	const std::optional<std::string> Parent = ParentMessageId.IsSet()
		? std::optional<std::string>(HaivenChatDetail::ToUtf8(ParentMessageId.GetValue()))
		: std::nullopt;
	OutMessage = ToBlueprint(Core.Send(HaivenChatDetail::ToUtf8(ChannelId), HaivenChatDetail::ToUtf8(Content), Parent));
	return true;
}

TArray<FHaivenOutgoingChatMessage> FHaivenChat::Outbox() const
{
	TArray<FHaivenOutgoingChatMessage> Result;
	for (const Haiven::FOutgoingChatMessage& Message : Core.Outbox()) Result.Add(ToBlueprint(Message));
	return Result;
}

void FHaivenChat::Forget(const FString& ClientMessageId)
{
	Core.Forget(HaivenChatDetail::ToUtf8(ClientMessageId));
}

FHaivenOutgoingChatMessage FHaivenChat::ToBlueprint(const Haiven::FOutgoingChatMessage& Message)
{
	FHaivenOutgoingChatMessage Result;
	Result.ClientMessageId = HaivenChatDetail::FromUtf8(Message.ClientMessageId);
	Result.ChannelId = HaivenChatDetail::FromUtf8(Message.ChannelId);
	Result.Content = HaivenChatDetail::FromUtf8(Message.Content);
	HaivenChatDetail::Assign(Message.ParentMessageId, Result.bHasParentMessageId, Result.ParentMessageId);
	Result.State = ToBlueprint(Message.State);
	HaivenChatDetail::Assign(Message.MessageId, Result.bHasMessageId, Result.MessageId);
	HaivenChatDetail::Assign(Message.ModerationState, Result.bHasModerationState, Result.ModerationState);
	HaivenChatDetail::Assign(Message.Error, Result.bHasError, Result.Error);
	return Result;
}

EHaivenOutgoingChatState FHaivenChat::ToBlueprint(Haiven::EOutgoingChatState State)
{
	switch (State)
	{
	case Haiven::EOutgoingChatState::Sending: return EHaivenOutgoingChatState::Sending;
	case Haiven::EOutgoingChatState::Unconfirmed: return EHaivenOutgoingChatState::Unconfirmed;
	case Haiven::EOutgoingChatState::Accepted: return EHaivenOutgoingChatState::Accepted;
	case Haiven::EOutgoingChatState::Held: return EHaivenOutgoingChatState::Held;
	case Haiven::EOutgoingChatState::Removed: return EHaivenOutgoingChatState::Removed;
	case Haiven::EOutgoingChatState::Refused: return EHaivenOutgoingChatState::Refused;
	}
	return EHaivenOutgoingChatState::Held;
}

UHaivenSendChatMessageAsyncAction* UHaivenSendChatMessageAsyncAction::SendChatMessage(
	UObject* WorldContextObject,
	UHaivenSubsystem* Subsystem,
	const FString& ChannelId,
	const FString& Content,
	bool bHasParentMessageId,
	const FString& ParentMessageId)
{
	UHaivenSendChatMessageAsyncAction* Action = NewObject<UHaivenSendChatMessageAsyncAction>();
	Action->Owner = Subsystem;
	Action->Channel = ChannelId;
	Action->Text = Content;
	if (bHasParentMessageId) Action->Parent = ParentMessageId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UHaivenSendChatMessageAsyncAction::Activate()
{
	if (!Owner)
	{
		Failed.Broadcast(TEXT("A Haiven subsystem is required."));
		SetReadyToDestroy();
		return;
	}
	FHaivenOutgoingChatMessage Message;
	FString Error;
	if (!Owner->SendChatMessage(Channel, Text, Parent.IsSet(), Parent.Get(FString()), Message, Error))
	{
		Failed.Broadcast(Error);
		SetReadyToDestroy();
		return;
	}
	// A final state only ever arrives with a later reply, so subscribing after
	// the send misses nothing.
	ClientMessageId = Message.ClientMessageId;
	Owner->OnOutgoingChatChanged.AddDynamic(this, &UHaivenSendChatMessageAsyncAction::HandleChanged);
	Sent.Broadcast(Message);
}

void UHaivenSendChatMessageAsyncAction::Cancel()
{
	Finish();
}

void UHaivenSendChatMessageAsyncAction::HandleChanged(const FHaivenOutgoingChatMessage& Message)
{
	if (Message.ClientMessageId != ClientMessageId || !HaivenChatDetail::IsFinal(Message.State)) return;
	if (Message.State == EHaivenOutgoingChatState::Refused) Refused.Broadcast(Message);
	else Settled.Broadcast(Message);
	Finish();
}

void UHaivenSendChatMessageAsyncAction::Finish()
{
	if (Owner) Owner->OnOutgoingChatChanged.RemoveDynamic(this, &UHaivenSendChatMessageAsyncAction::HandleChanged);
	SetReadyToDestroy();
}
