#pragma once

#include "Contracts/HaivenRestJson.h"
#include "HaivenJson.h"
#include "HaivenRestApi.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Haiven
{
	enum class EAnalyticsDiagnosticKind
	{
		QueueFull,
		RetryScheduled,
		BatchDropped,
		EventRejected,
	};

	struct FAnalyticsDiagnostic
	{
		EAnalyticsDiagnosticKind Kind = EAnalyticsDiagnosticKind::BatchDropped;
		std::vector<std::string> EventIds;
		int Attempt = 0;
		std::uint64_t DelayMilliseconds = 0;
		std::string Reason;
	};

	struct FAnalyticsBatcherOptions
	{
		std::size_t BatchSize = 100;
		std::size_t MaxBufferedEvents = 1000;
		int MaxRetries = 3;
		std::uint64_t FlushIntervalMilliseconds = 1000;
		std::uint64_t InitialRetryDelayMilliseconds = 1000;
		std::uint64_t MaxRetryDelayMilliseconds = 30000;

		bool Validate(std::string& Error) const
		{
			if (BatchSize < 1 || BatchSize > 100)
				Error = "haiven: analytics batch size must be 1 to 100";
			else if (MaxBufferedEvents < BatchSize)
				Error = "haiven: analytics buffer must hold at least one batch";
			else if (MaxRetries < 0)
				Error = "haiven: analytics max retries cannot be negative";
			else if (InitialRetryDelayMilliseconds == 0 || MaxRetryDelayMilliseconds < InitialRetryDelayMilliseconds)
				Error = "haiven: analytics retry delays are invalid";
			else
			{
				Error.clear();
				return true;
			}
			return false;
		}
	};

	struct FAnalyticsDispatchResult
	{
		enum class EAction { Delivered, Retry, Dropped };
		EAction Action = EAction::Dropped;
		std::uint64_t RetryAfterMilliseconds = 0;
		std::vector<FAnalyticsDiagnostic> Diagnostics;
	};

	/**
	 * A bounded analytics state machine shared by native hosts. It performs no I/O,
	 * starts no threads, and owns no engine objects. Hosts enqueue on their gameplay
	 * thread, defer BeginBatch, send CurrentRequest asynchronously, and feed the reply
	 * to HandleReply. The exact serialized batch remains stable across every retry.
	 * Invalid configuration and input are reported as finite state/results; this core
	 * does not require C++ exceptions.
	 */
	class FAnalyticsBatcherCore
	{
	public:
		using FValueSource = std::function<std::string()>;

		FAnalyticsBatcherCore(
			RestTypes::AnalyticsContext InContext,
			FAnalyticsBatcherOptions InOptions,
			std::string InAnalyticsSessionId,
			FValueSource InNextEventId,
			FValueSource InUtcNow)
			: Context(std::move(InContext))
			, Options(InOptions)
			, AnalyticsSessionId(std::move(InAnalyticsSessionId))
			, NextEventId(std::move(InNextEventId))
			, UtcNow(std::move(InUtcNow))
		{
			if (!Options.Validate(ConfigurationError)) return;
			if (AnalyticsSessionId.empty())
				ConfigurationError = "haiven: an analytics session id is required";
			else if (!NextEventId || !UtcNow)
				ConfigurationError = "haiven: analytics id and clock sources are required";
		}

		bool TryEnqueue(RestTypes::AnalyticsEvent Event, std::optional<FAnalyticsDiagnostic>& Diagnostic)
		{
			Diagnostic.reset();
			if (!IsValid()) return Reject(Diagnostic, {}, ConfigurationError);
			if (bDisposed) return Reject(Diagnostic, {}, "analytics batcher is disposed");
			if (!Event.Type || !IsEventType(*Event.Type))
				return Reject(Diagnostic, EventIdList(Event), "analytics event type is not in the published contract");

			std::string PropertiesError;
			if (!ValidateProperties(Event, PropertiesError))
				return Reject(Diagnostic, EventIdList(Event), std::move(PropertiesError));

			if (!Event.EventId || Event.EventId->empty()) Event.EventId = NextEventId();
			if (!Event.EventId || Event.EventId->empty())
				return Reject(Diagnostic, {}, "analytics event id source returned an empty value");
			if (!Event.AnalyticsSessionId || Event.AnalyticsSessionId->empty()) Event.AnalyticsSessionId = AnalyticsSessionId;
			if (!Event.OccurredAt || Event.OccurredAt->empty()) Event.OccurredAt = UtcNow();
			if (!Event.OccurredAt || Event.OccurredAt->empty())
				return Reject(Diagnostic, EventIdList(Event), "analytics clock source returned an empty value");
			if (BufferedEventCount() >= Options.MaxBufferedEvents)
			{
				++DroppedEvents;
				Diagnostic = MakeDiagnostic(EAnalyticsDiagnosticKind::QueueFull, EventIdList(Event), "analytics queue is full");
				return false;
			}
			Queue.push_back(std::move(Event));
			return true;
		}

		bool TryEnqueue(RestTypes::AnalyticsEvent Event)
		{
			std::optional<FAnalyticsDiagnostic> Ignored;
			return TryEnqueue(std::move(Event), Ignored);
		}

		std::optional<Rest::FRestRequest> BeginBatch()
		{
			if (!IsValid() || bDisposed) return std::nullopt;
			if (!InFlight.empty()) return CurrentRequest;
			if (Queue.empty()) return std::nullopt;
			const std::size_t Count = std::min(Options.BatchSize, Queue.size());
			InFlight.reserve(Count);
			for (std::size_t Index = 0; Index < Count; ++Index)
			{
				InFlight.push_back(std::move(Queue.front()));
				Queue.pop_front();
			}
			RestTypes::IngestAnalyticsRequest Body;
			Body.Context = Context;
			Body.Events = InFlight;
			CurrentRequest = Rest::PostSdkAnalyticsEvents(ContractsJson::ToJson(Body));
			SendAttempts = 1;
			return CurrentRequest;
		}

		FAnalyticsDispatchResult HandleReply(int Status, const std::string& Body, const std::string& TransportError = {})
		{
			if (!IsValid()) return InvalidDispatch(ConfigurationError);
			if (bDisposed) return InvalidDispatch("analytics batcher is disposed");
			if (InFlight.empty()) return InvalidDispatch("analytics reply has no in-flight batch");
			if (Status >= 200 && Status < 300)
			{
				RestTypes::IngestAnalyticsResponse Response;
				std::string ParseError;
				if (!Body.empty() && ContractsJson::FromJson(Body, Response, ParseError))
				{
					FAnalyticsDispatchResult Result;
					Result.Action = FAnalyticsDispatchResult::EAction::Delivered;
					if (Response.Rejected)
					{
						for (const RestTypes::AnalyticsRejection& Rejection : *Response.Rejected)
						{
							std::vector<std::string> Ids;
							if (Rejection.Index && *Rejection.Index >= 0
								&& static_cast<std::size_t>(*Rejection.Index) < InFlight.size())
								Ids.push_back(EventId(InFlight[static_cast<std::size_t>(*Rejection.Index)]));
							Result.Diagnostics.push_back(MakeDiagnostic(
								EAnalyticsDiagnosticKind::EventRejected,
								std::move(Ids),
								Rejection.Reason.value_or("server rejected analytics event")));
						}
					}
					ClearInFlight();
					return Result;
				}
				const std::string Reason = Body.empty()
					? "analytics success reply was empty"
					: "analytics success reply could not be read: " + ParseError;
				return HandleFailure(Reason, true);
			}
			const bool bRetryable = Status == 0 || Status == 429 || Status >= 500;
			const std::string Reason = !TransportError.empty()
				? TransportError
				: "analytics request failed with HTTP " + std::to_string(Status);
			return HandleFailure(Reason, bRetryable);
		}

		std::vector<FAnalyticsDiagnostic> Dispose()
		{
			if (bDisposed) return {};
			bDisposed = true;
			std::vector<std::string> Ids = CurrentEventIds();
			for (const auto& Event : Queue) Ids.push_back(EventId(Event));
			DroppedEvents += Ids.size();
			Queue.clear();
			ClearInFlight();
			if (Ids.empty()) return {};
			return {MakeDiagnostic(EAnalyticsDiagnosticKind::BatchDropped, std::move(Ids), "batcher disposed")};
		}

		bool IsValid() const { return ConfigurationError.empty(); }
		const std::string& GetConfigurationError() const { return ConfigurationError; }
		bool IsIdle() const { return Queue.empty() && InFlight.empty(); }
		std::size_t BufferedEventCount() const { return Queue.size() + InFlight.size(); }
		std::size_t QueuedEventCount() const { return Queue.size(); }
		std::size_t InFlightEventCount() const { return InFlight.size(); }
		std::uint64_t DroppedEventCount() const { return DroppedEvents; }
		std::uint64_t RetryCount() const { return Retries; }
		bool IsDisposed() const { return bDisposed; }
		const FAnalyticsBatcherOptions& GetOptions() const { return Options; }

	private:
		static bool IsEventType(const std::string& Type)
		{
			return Type == "game.launch" || Type == "session.start" || Type == "session.end"
				|| Type == "playtime" || Type == "sdk.health";
		}

		static bool ValidateProperties(RestTypes::AnalyticsEvent& Event, std::string& Error)
		{
			const std::string Raw = Event.Properties.value_or("{}");
			FJson Parsed;
			if (!FJson::Parse(Raw, Parsed, Error))
			{
				Error = "analytics properties are malformed JSON: " + Error;
				return false;
			}
			if (!Parsed.IsObject())
			{
				Error = "analytics properties must be a JSON object";
				return false;
			}
			if (Parsed.Members.size() > 32)
			{
				Error = "analytics properties may contain at most 32 members";
				return false;
			}
			for (const auto& Member : Parsed.Members)
			{
				const FJson::EKind Kind = Member.second.Kind;
				if (Kind == FJson::EKind::Array || Kind == FJson::EKind::Object)
				{
					Error = "analytics properties must contain only flat scalar values";
					return false;
				}
			}
			Event.Properties = Parsed.Dump();
			return true;
		}

		static bool Reject(
			std::optional<FAnalyticsDiagnostic>& Diagnostic,
			std::vector<std::string> Ids,
			std::string Reason)
		{
			Diagnostic = MakeDiagnostic(EAnalyticsDiagnosticKind::EventRejected, std::move(Ids), std::move(Reason));
			return false;
		}

		static FAnalyticsDispatchResult InvalidDispatch(std::string Reason)
		{
			FAnalyticsDispatchResult Result;
			Result.Action = FAnalyticsDispatchResult::EAction::Dropped;
			Result.Diagnostics.push_back(MakeDiagnostic(EAnalyticsDiagnosticKind::BatchDropped, {}, std::move(Reason)));
			return Result;
		}

		FAnalyticsDispatchResult HandleFailure(std::string Reason, bool bRetryable)
		{
			const std::vector<std::string> Ids = CurrentEventIds();
			if (bRetryable && SendAttempts <= Options.MaxRetries)
			{
				const int RetryAttempt = SendAttempts++;
				const std::uint64_t Delay = Backoff(RetryAttempt);
				++Retries;
				FAnalyticsDispatchResult Result;
				Result.Action = FAnalyticsDispatchResult::EAction::Retry;
				Result.RetryAfterMilliseconds = Delay;
				FAnalyticsDiagnostic Diagnostic = MakeDiagnostic(
					EAnalyticsDiagnosticKind::RetryScheduled, Ids, std::move(Reason));
				Diagnostic.Attempt = RetryAttempt;
				Diagnostic.DelayMilliseconds = Delay;
				Result.Diagnostics.push_back(std::move(Diagnostic));
				return Result;
			}
			DroppedEvents += InFlight.size();
			FAnalyticsDispatchResult Result;
			Result.Action = FAnalyticsDispatchResult::EAction::Dropped;
			FAnalyticsDiagnostic Diagnostic = MakeDiagnostic(
				EAnalyticsDiagnosticKind::BatchDropped, Ids,
				bRetryable ? "retry limit reached: " + Reason : std::move(Reason));
			Diagnostic.Attempt = SendAttempts;
			Result.Diagnostics.push_back(std::move(Diagnostic));
			ClearInFlight();
			return Result;
		}

		std::uint64_t Backoff(int RetryAttempt) const
		{
			std::uint64_t Delay = Options.InitialRetryDelayMilliseconds;
			for (int Index = 1; Index < RetryAttempt && Delay < Options.MaxRetryDelayMilliseconds; ++Index)
			{
				if (Delay > Options.MaxRetryDelayMilliseconds / 2) return Options.MaxRetryDelayMilliseconds;
				Delay *= 2;
			}
			return std::min(Delay, Options.MaxRetryDelayMilliseconds);
		}

		static std::string EventId(const RestTypes::AnalyticsEvent& Event)
		{
			return Event.EventId.value_or(std::string());
		}

		static std::vector<std::string> EventIdList(const RestTypes::AnalyticsEvent& Event)
		{
			const std::string Id = EventId(Event);
			return Id.empty() ? std::vector<std::string>() : std::vector<std::string>{Id};
		}

		std::vector<std::string> CurrentEventIds() const
		{
			std::vector<std::string> Ids;
			Ids.reserve(InFlight.size());
			for (const auto& Event : InFlight) Ids.push_back(EventId(Event));
			return Ids;
		}

		static FAnalyticsDiagnostic MakeDiagnostic(
			EAnalyticsDiagnosticKind Kind,
			std::vector<std::string> Ids,
			std::string Reason)
		{
			FAnalyticsDiagnostic Result;
			Result.Kind = Kind;
			Result.EventIds = std::move(Ids);
			Result.Reason = std::move(Reason);
			return Result;
		}

		void ClearInFlight()
		{
			InFlight.clear();
			CurrentRequest.reset();
			SendAttempts = 0;
		}

		RestTypes::AnalyticsContext Context;
		FAnalyticsBatcherOptions Options;
		std::string AnalyticsSessionId;
		FValueSource NextEventId;
		FValueSource UtcNow;
		std::string ConfigurationError;
		std::deque<RestTypes::AnalyticsEvent> Queue;
		std::vector<RestTypes::AnalyticsEvent> InFlight;
		std::optional<Rest::FRestRequest> CurrentRequest;
		int SendAttempts = 0;
		std::uint64_t DroppedEvents = 0;
		std::uint64_t Retries = 0;
		bool bDisposed = false;
	};
}
