#include "HaivenHealth.h"

DEFINE_LOG_CATEGORY(LogHaiven);

void FHaivenHealth::TallyChatOutbox(const TArray<FHaivenOutgoingChatMessage>& Outbox)
{
	for (const FHaivenOutgoingChatMessage& Message : Outbox)
	{
		switch (Message.State)
		{
		case EHaivenOutgoingChatState::Sending: ++ChatSending; break;
		case EHaivenOutgoingChatState::Unconfirmed: ++ChatUnconfirmed; break;
		case EHaivenOutgoingChatState::Accepted: ++ChatAccepted; break;
		case EHaivenOutgoingChatState::Held: ++ChatHeld; break;
		case EHaivenOutgoingChatState::Removed: ++ChatRemoved; break;
		case EHaivenOutgoingChatState::Refused: ++ChatRefused; break;
		}
	}
}
