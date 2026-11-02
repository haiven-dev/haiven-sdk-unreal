#pragma once

#include "HaivenProtocol.h"

#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Haiven
{
	enum class ESessionState
	{
		Pending,
		Authenticating,
		Ready,
		Expired,
		Closed,
	};

	struct FClientFrame
	{
		std::string Type;
		std::string RequestId;
		std::optional<std::string> PayloadJson;
	};

	/**
	 * Engine-neutral realtime state machine. The Unreal layer owns IWebSocket,
	 * JSON conversion, and game-thread dispatch; this core owns the ordering
	 * rules which must remain identical across engine transports.
	 */
	class FSessionCore
	{
	public:
		using FWrite = std::function<void(const FClientFrame&)>;
		using FTokenSource = std::function<std::string()>;
		using FRequestIdSource = std::function<std::string()>;
		using FFrameHandler = std::function<void(const FServerFrame&, const std::string&)>;
		using FStateHandler = std::function<void(ESessionState)>;
		using FCloseHandler = std::function<void(int, const std::string&)>;

		FSessionCore(FWrite InWrite, FTokenSource InTokenSource, FRequestIdSource InRequestIdSource)
			: Write(std::move(InWrite))
			, TokenSource(std::move(InTokenSource))
			, RequestIdSource(std::move(InRequestIdSource))
		{
			if (!Write || !TokenSource || !RequestIdSource)
			{
				throw std::invalid_argument("haiven: session callbacks are required");
			}
		}

		ESessionState GetState() const { return State; }

		void Opened()
		{
			if (State == ESessionState::Closed)
			{
				throw std::logic_error("haiven: cannot open a closed session");
			}
			if (bOpened) return;
			bOpened = true;
			SetState(ESessionState::Authenticating);
			Authenticate();
		}

		std::string Send(std::string Type, std::optional<std::string> PayloadJson = std::nullopt)
		{
			if (State == ESessionState::Closed)
			{
				throw std::logic_error("haiven: send on a closed session");
			}

			FClientFrame Frame{std::move(Type), RequestIdSource(), std::move(PayloadJson)};
			const std::string RequestId = Frame.RequestId;
			if (State == ESessionState::Ready)
			{
				WriteApplication(std::move(Frame));
			}
			else
			{
				Pending.push_back(std::move(Frame));
			}
			return RequestId;
		}

		void Receive(const FServerFrame& Frame, const std::string& RawJson)
		{
			if (State == ESessionState::Closed) return;
			if (OnFrame) OnFrame(Frame, RawJson);

			if (Frame.Type == AuthSuccessType)
			{
				const bool bStateChanged = State != ESessionState::Ready;
				State = ESessionState::Ready;
				while (!Pending.empty())
				{
					FClientFrame FrameToWrite = std::move(Pending.front());
					Pending.pop_front();
					WriteApplication(std::move(FrameToWrite));
				}
				// Observers can send synchronously. Publish readiness only after the
				// older queued/replayed frames are back on the wire, preserving order.
				if (bStateChanged && OnStateChanged) OnStateChanged(State);
				if (OnAuthenticated) OnAuthenticated();
				return;
			}

			if (Frame.Type == AuthExpiredType)
			{
				if (State != ESessionState::Expired)
				{
					RequeueInFlight();
					SetState(ESessionState::Expired);
					if (OnExpired) OnExpired();
					Authenticate();
				}
				return;
			}

			if (IsReply(Frame))
			{
				if (Frame.RequestId.has_value()) RemoveOutstanding(*Frame.RequestId);
				if (OnReply) OnReply(Frame, RawJson);
			}
			else if (OnEvent)
			{
				OnEvent(Frame, RawJson);
			}
		}

		void Closed(int Code, std::string Reason)
		{
			if (State == ESessionState::Closed) return;
			SetState(ESessionState::Closed);
			Pending.clear();
			InFlight.clear();
			if (OnClosed) OnClosed(Code, Reason);
		}

		FFrameHandler OnFrame;
		FFrameHandler OnReply;
		FFrameHandler OnEvent;
		std::function<void()> OnAuthenticated;
		std::function<void()> OnExpired;
		FStateHandler OnStateChanged;
		FCloseHandler OnClosed;

	private:
		void Authenticate()
		{
			Write(FClientFrame{AuthType, RequestIdSource(), TokenSource()});
		}

		void WriteApplication(FClientFrame Frame)
		{
			InFlight.push_back(Frame);
			Write(Frame);
		}

		void RequeueInFlight()
		{
			std::deque<FClientFrame> Replay;
			Replay.swap(InFlight);
			while (!Pending.empty())
			{
				Replay.push_back(std::move(Pending.front()));
				Pending.pop_front();
			}
			Pending.swap(Replay);
		}

		void RemoveOutstanding(const std::string& RequestId)
		{
			for (auto It = InFlight.begin(); It != InFlight.end(); ++It)
			{
				if (It->RequestId != RequestId) continue;
				InFlight.erase(It);
				return;
			}
			// A late reply can arrive after auth.expired moved this frame back to
			// Pending but before re-auth succeeds. Do not replay work already answered.
			for (auto It = Pending.begin(); It != Pending.end(); ++It)
			{
				if (It->RequestId != RequestId) continue;
				Pending.erase(It);
				return;
			}
		}

		void SetState(ESessionState NewState)
		{
			if (State == NewState) return;
			State = NewState;
			if (OnStateChanged) OnStateChanged(State);
		}

		FWrite Write;
		FTokenSource TokenSource;
		FRequestIdSource RequestIdSource;
		std::deque<FClientFrame> Pending;
		std::deque<FClientFrame> InFlight;
		ESessionState State = ESessionState::Pending;
		bool bOpened = false;
	};
}
