#pragma once

#include "HaivenSessionCore.h"

#include <cstdio>
#include <functional>
#include <list>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Haiven
{
	/** Where a message this player sent stands. */
	enum class EOutgoingChatState
	{
		/** Written, or queued until the session authenticates; no reply yet. */
		Sending,
		/** The socket closed before the reply. Attaching the next session resends it. */
		Unconfirmed,
		/** Accepted and visible to the channel (`allowed` or `warned`). */
		Accepted,
		/** Accepted but held for moderation (`pending` or `review_pending`). */
		Held,
		/** Accepted but not shown (`hidden` or `blocked`). */
		Removed,
		/** Refused with an error code; resending the same message cannot succeed. */
		Refused,
	};

	/** One message this player sent, tracked by its client message id. */
	struct FOutgoingChatMessage
	{
		/** The deduplication key: a resend under it is the same message to the server. */
		std::string ClientMessageId;
		std::string ChannelId;
		std::string Content;
		std::optional<std::string> ParentMessageId;
		EOutgoingChatState State = EOutgoingChatState::Sending;
		std::optional<std::string> MessageId;
		std::optional<std::string> ModerationState;
		std::optional<std::string> Error;
	};

	/**
	 * Headless chat over FSessionCore: the engine-free half of the plugin's chat.
	 * The C++ counterpart of the .NET core's HaivenChat.
	 *
	 * A session is one socket, so this outlives sessions: Attach each session the
	 * game opens, including after a reconnect. Every send carries a client
	 * message id; a send unanswered when its socket closes becomes Unconfirmed,
	 * and the next Attach resends it under the same id, which haiven-api
	 * deduplicates. Nothing here throws on a network failure: a send without a
	 * usable session is kept and resent on attach.
	 *
	 * The core has no JSON parser, like the session core: the engine layer
	 * decodes a chat.send reply (ChatMessageSubmissionV0) and passes its
	 * moderation state and message id to HandleReply, and routes the session's
	 * reply and close callbacks here. It builds only its own small request body.
	 */
	class FChatCore
	{
	public:
		using FClientMessageIdSource = std::function<std::string()>;

		explicit FChatCore(FClientMessageIdSource InNewClientMessageId)
			: NewClientMessageId(std::move(InNewClientMessageId))
		{
			if (!NewClientMessageId) throw std::invalid_argument("haiven: a client message id source is required");
		}

		/** Raised whenever an outgoing message changes state. */
		std::function<void(const FOutgoingChatMessage&)> OnOutgoingChanged;

		/** Messages this player sent, oldest first, until Forget. */
		const std::list<FOutgoingChatMessage>& Outbox() const { return Messages; }

		/** Uses this session from now on, and resends every unconfirmed message through it. */
		void Attach(FSessionCore& Session)
		{
			Current = &Session;
			ByRequestId.clear();
			for (FOutgoingChatMessage& Message : Messages)
			{
				if (Message.State == EOutgoingChatState::Unconfirmed) Write(Message);
			}
		}

		/** Sends a message, or keeps it to send on the next attach. */
		const FOutgoingChatMessage& Send(std::string ChannelId, std::string Content, std::optional<std::string> ParentMessageId = std::nullopt)
		{
			if (ChannelId.empty()) throw std::invalid_argument("haiven: channelId is required");
			FOutgoingChatMessage& Message = Messages.emplace_back();
			Message.ClientMessageId = NewClientMessageId();
			Message.ChannelId = std::move(ChannelId);
			Message.Content = std::move(Content);
			Message.ParentMessageId = std::move(ParentMessageId);
			if (Usable()) Write(Message);
			else Change(Message, EOutgoingChatState::Unconfirmed);
			return Message;
		}

		/**
		 * A reply from the attached session. ModerationState and MessageId come
		 * from the decoded ChatMessageSubmissionV0 of a successful reply. A reply
		 * to any other request is ignored.
		 */
		void HandleReply(const FServerFrame& Frame, std::optional<std::string> ModerationState, std::optional<std::string> MessageId)
		{
			if (!Frame.RequestId.has_value()) return;
			const auto Found = ByRequestId.find(*Frame.RequestId);
			if (Found == ByRequestId.end()) return;
			FOutgoingChatMessage& Message = *Found->second;
			ByRequestId.erase(Found);
			if (!Succeeded(Frame))
			{
				Message.Error = Frame.Error;
				Change(Message, EOutgoingChatState::Refused);
				return;
			}
			Message.ModerationState = ModerationState;
			Message.MessageId = std::move(MessageId);
			Change(Message, StateFor(ModerationState));
		}

		/** The attached session's socket closed: unanswered sends become unconfirmed. */
		void HandleClosed()
		{
			std::vector<FOutgoingChatMessage*> Unanswered;
			for (auto& Entry : ByRequestId) Unanswered.push_back(Entry.second);
			ByRequestId.clear();
			Current = nullptr;
			for (FOutgoingChatMessage* Message : Unanswered) Change(*Message, EOutgoingChatState::Unconfirmed);
		}

		/** Stops tracking a message, normally one in a final state. */
		void Forget(const std::string& ClientMessageId)
		{
			for (auto It = ByRequestId.begin(); It != ByRequestId.end();)
			{
				if (It->second->ClientMessageId == ClientMessageId) It = ByRequestId.erase(It);
				else ++It;
			}
			Messages.remove_if([&](const FOutgoingChatMessage& Message) { return Message.ClientMessageId == ClientMessageId; });
		}

		/** A moderation state minted after this build reads as held, never as visible. */
		static EOutgoingChatState StateFor(const std::optional<std::string>& ModerationState)
		{
			if (ModerationState == "allowed" || ModerationState == "warned") return EOutgoingChatState::Accepted;
			if (ModerationState == "hidden" || ModerationState == "blocked") return EOutgoingChatState::Removed;
			return EOutgoingChatState::Held;
		}

		/** The chat.send body (ChatSendRequestV0) for a message. */
		static std::string SendPayloadJson(const FOutgoingChatMessage& Message)
		{
			std::string Json = "{\"channelId\":" + Quote(Message.ChannelId);
			if (Message.ParentMessageId.has_value()) Json += ",\"parentMessageId\":" + Quote(*Message.ParentMessageId);
			Json += ",\"clientMessageId\":" + Quote(Message.ClientMessageId);
			Json += ",\"content\":" + Quote(Message.Content) + "}";
			return Json;
		}

		/** A JSON string literal: quotes, backslashes and control characters escaped. */
		static std::string Quote(const std::string& Value)
		{
			std::string Out = "\"";
			for (const char Raw : Value)
			{
				const unsigned char C = static_cast<unsigned char>(Raw);
				switch (C)
				{
				case '"': Out += "\\\""; break;
				case '\\': Out += "\\\\"; break;
				case '\b': Out += "\\b"; break;
				case '\f': Out += "\\f"; break;
				case '\n': Out += "\\n"; break;
				case '\r': Out += "\\r"; break;
				case '\t': Out += "\\t"; break;
				default:
					if (C < 0x20)
					{
						char Escaped[7];
						std::snprintf(Escaped, sizeof(Escaped), "\\u%04x", C);
						Out += Escaped;
					}
					else
					{
						// UTF-8 bytes pass through unchanged: JSON text is UTF-8.
						Out += Raw;
					}
				}
			}
			return Out + "\"";
		}

	private:
		bool Usable() const { return Current != nullptr && Current->GetState() != ESessionState::Closed; }

		void Write(FOutgoingChatMessage& Message)
		{
			const std::string RequestId = Current->Send("chat.send", SendPayloadJson(Message));
			ByRequestId[RequestId] = &Message;
			Change(Message, EOutgoingChatState::Sending);
		}

		void Change(FOutgoingChatMessage& Message, EOutgoingChatState NewState)
		{
			Message.State = NewState;
			if (OnOutgoingChanged) OnOutgoingChanged(Message);
		}

		FClientMessageIdSource NewClientMessageId;
		// A list, so a reference to a message stays valid as others are added or forgotten.
		std::list<FOutgoingChatMessage> Messages;
		std::unordered_map<std::string, FOutgoingChatMessage*> ByRequestId;
		FSessionCore* Current = nullptr;
	};
}
