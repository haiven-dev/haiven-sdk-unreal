#pragma once

#include "HaivenCredentialLifecycle.h"
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

	/**
	 * What a socket close means for the credential that opened it. Finite and
	 * SDK-local: the server sends a close code and a reason string, and this is
	 * the part of them a game acts on.
	 */
	enum class ECloseKind
	{
		/// The connection ended for a reason unrelated to the credential: network
		/// loss, a server restart, or the caller's own close. The credential is
		/// still good, and a new socket may authenticate with it.
		Transport,
		/// `1008 unauthorized`: the server refused the credential in the handshake.
		CredentialRefused,
		/// `1008 credential_revoked`: the player or an operator revoked the game
		/// credential while the socket was open.
		CredentialRevoked,
		/// Any other `1008`, for example `parental_consent_denied`. The server
		/// refused this socket for a reason a new socket with the same credential
		/// does not change.
		Policy,
	};

	/// `websocket.StatusPolicyViolation`: every close the server makes on purpose.
	inline constexpr int PolicyViolationCloseCode = 1008;

	/// Classifies a close by its code and reason alone, identically in every SDK.
	inline ECloseKind ClassifyClose(int Code, const std::string& Reason)
	{
		if (Code != PolicyViolationCloseCode) return ECloseKind::Transport;
		if (Reason == "unauthorized") return ECloseKind::CredentialRefused;
		if (Reason == "credential_revoked") return ECloseKind::CredentialRevoked;
		return ECloseKind::Policy;
	}

	/// True when the credential will not authenticate again: discard it and run
	/// the game login handoff before opening another socket.
	inline bool CloseRequiresSignIn(ECloseKind Kind)
	{
		return Kind == ECloseKind::CredentialRefused || Kind == ECloseKind::CredentialRevoked;
	}

	/// True when a new socket may authenticate with the same credential. Only a
	/// transport close qualifies; reconnecting after any `1008` repeats the refusal.
	inline bool CloseMayReconnect(ECloseKind Kind) { return Kind == ECloseKind::Transport; }

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
		using FCredentialActionHandler = std::function<void(ECredentialAction)>;

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

		/// What the close means for the credential, once the session is closed;
		/// empty before. Set before `OnStateChanged` and `OnClosed` run.
		std::optional<ECloseKind> GetCloseKind() const { return CloseKind; }

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
				// A server that parks outbound frames replays the handshake reply after
				// the re-auth that released it; on a ready session it changes nothing.
				if (State == ESessionState::Ready) return;
				State = ESessionState::Ready;
				std::deque<FClientFrame> Queued;
				Queued.swap(AwaitingResend);
				while (!Pending.empty())
				{
					Queued.push_back(std::move(Pending.front()));
					Pending.pop_front();
				}
				for (FClientFrame& FrameToWrite : Queued) WriteApplication(std::move(FrameToWrite));
				// Observers can send synchronously. Publish readiness only after the
				// older queued/replayed frames are back on the wire, preserving order.
				if (OnStateChanged) OnStateChanged(State);
				if (OnAuthenticated) OnAuthenticated();
				return;
			}

			if (Frame.Type == AuthExpiredType)
			{
				HandleExpired(Frame);
				return;
			}

			if (Frame.Type == AuthFailureType)
			{
				// Same-socket refusal is not proof the credential is invalid. A
				// fresh first-frame handshake is the authoritative check.
				SetState(ESessionState::Expired);
				const ECredentialAction CredentialAction =
					FCredentialLifecycle::ForAuthenticationFailure(Frame.Error.value_or(""));
				if (CredentialAction != ECredentialAction::Retain && OnCredentialActionRequired)
					OnCredentialActionRequired(CredentialAction);
				if (OnAuthenticationFailed) OnAuthenticationFailed(Frame.Error.value_or(""));
				return;
			}

			if (Frame.Type == AuthExpiredDroppedType)
			{
				if (Frame.Dropped.has_value()) HandleDropped(*Frame.Dropped);
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
			const ESessionState PreviousState = State;
			CloseKind = ClassifyClose(Code, Reason);
			SetState(ESessionState::Closed);
			Pending.clear();
			InFlight.clear();
			AwaitingResend.clear();
			const ECredentialSocketPhase Phase = PreviousState == ESessionState::Pending
				|| PreviousState == ESessionState::Authenticating
				? ECredentialSocketPhase::InitialAuthentication
				: ECredentialSocketPhase::Live;
			const ECredentialAction CredentialAction = FCredentialLifecycle::ForSocketClose(Phase, Code, Reason);
			if (CredentialAction != ECredentialAction::Retain && OnCredentialActionRequired)
				OnCredentialActionRequired(CredentialAction);
			if (OnClosed) OnClosed(Code, Reason);
		}

		FFrameHandler OnFrame;
		FFrameHandler OnReply;
		FFrameHandler OnEvent;
		std::function<void()> OnAuthenticated;
		std::function<void()> OnExpired;
		/// `auth.failure` on a live socket; OnCredentialActionRequired says whether to retry fresh.
		std::function<void(const std::string&)> OnAuthenticationFailed;
		FCredentialActionHandler OnCredentialActionRequired;
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

		/// Removes and returns the unanswered request with this id, if any.
		std::optional<FClientFrame> TakeInFlight(const std::string& RequestId)
		{
			for (auto It = InFlight.begin(); It != InFlight.end(); ++It)
			{
				if (It->RequestId != RequestId) continue;
				FClientFrame Frame = std::move(*It);
				InFlight.erase(It);
				return Frame;
			}
			return std::nullopt;
		}

		/// Only what the server names is resent. A request it accepted before the
		/// lapse has its reply parked and replayed, so resending it would duplicate
		/// it (spec/client.md, SDK-RECOVERY-2/3).
		void HandleExpired(const FServerFrame& Frame)
		{
			if (Frame.RequestId.has_value() && !Frame.RequestId->empty())
			{
				if (std::optional<FClientFrame> Refused = TakeInFlight(*Frame.RequestId))
				{
					AwaitingResend.push_back(std::move(*Refused));
				}
			}
			// One re-auth per episode, on the same socket.
			if (State == ESessionState::Expired) return;
			SetState(ESessionState::Expired);
			if (OnExpired) OnExpired();
			Authenticate();
		}

		/// The overflow half of an expiry. It arrives after the auth.success that
		/// ended the episode, so what it names goes out at once (SDK-RECOVERY-4/5).
		void HandleDropped(const FDroppedFrames& Report)
		{
			std::deque<FClientFrame> Named;
			for (const std::string& RequestId : Report.RequestIds)
			{
				if (std::optional<FClientFrame> Dropped = TakeInFlight(RequestId))
				{
					Named.push_back(std::move(*Dropped));
				}
			}
			// A truncated list is a sample: every request still unanswered is suspect.
			// Read before resending, because a resend is in flight again.
			std::deque<FClientFrame> Suspect;
			if (Report.bTruncated) Suspect.swap(InFlight);
			Requeue(std::move(Named));
			Requeue(std::move(Suspect));
		}

		/// Sends recovered requests now if the session is usable, else after the next auth.success.
		void Requeue(std::deque<FClientFrame> Frames)
		{
			for (FClientFrame& Frame : Frames)
			{
				if (State == ESessionState::Ready) WriteApplication(std::move(Frame));
				else AwaitingResend.push_back(std::move(Frame));
			}
		}

		void RemoveOutstanding(const std::string& RequestId)
		{
			for (auto It = InFlight.begin(); It != InFlight.end(); ++It)
			{
				if (It->RequestId != RequestId) continue;
				InFlight.erase(It);
				return;
			}
			// A late reply can arrive for a request held for resend before re-auth
			// succeeds. Do not replay work already answered.
			for (auto It = AwaitingResend.begin(); It != AwaitingResend.end(); ++It)
			{
				if (It->RequestId != RequestId) continue;
				AwaitingResend.erase(It);
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
		/// Written and not yet answered, oldest first.
		std::deque<FClientFrame> InFlight;
		/// Refused or dropped requests, written again after the next auth.success.
		std::deque<FClientFrame> AwaitingResend;
		ESessionState State = ESessionState::Pending;
		std::optional<ECloseKind> CloseKind;
		bool bOpened = false;
	};
}
