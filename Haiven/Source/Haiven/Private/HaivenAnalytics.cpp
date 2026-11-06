#include "HaivenAnalytics.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HaivenSubsystem.h"

#include <string>

namespace HaivenAnalyticsDetail
{
	std::string ToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value);
		return std::string(Converted.Get(), Converted.Length());
	}

	FString FromUtf8(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}

	FString NewId()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}
}

TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> FHaivenAnalytics::CreateForSubsystem(
	UHaivenSubsystem* Owner,
	Haiven::RestTypes::AnalyticsContext Context,
	Haiven::FAnalyticsBatcherOptions Options,
	FDiagnosticSink DiagnosticSink)
{
	TWeakObjectPtr<UHaivenSubsystem> WeakOwner(Owner);
	FSend SendAdapter = [WeakOwner](const FHaivenRestRequest& Request, FComplete Complete)
	{
		if (!WeakOwner.IsValid())
		{
			FHaivenHttpResult Result;
			Result.Error = TEXT("The Haiven subsystem is unavailable.");
			Complete(Result);
			return;
		}
		WeakOwner->Request(
			Request.Method,
			Request.Path,
			Request.Body,
			FHaivenHttpComplete::CreateLambda([Complete = MoveTemp(Complete)](const FHaivenHttpResult& Result) mutable
			{
				Complete(Result);
			}));
	};
	FSchedule ScheduleAdapter = [](double DelaySeconds, TFunction<void()> Callback)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Callback = MoveTemp(Callback)](float) mutable
			{
				Callback();
				return false;
			}),
			static_cast<float>(DelaySeconds));
	};
	FPost PostAdapter = [](TFunction<void()> Callback)
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Callback));
	};
	const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Result = Create(
		std::move(Context), Options, MoveTemp(SendAdapter), MoveTemp(ScheduleAdapter), MoveTemp(PostAdapter),
		MoveTemp(DiagnosticSink));
	if (Owner && IsInGameThread()) Owner->RegisterAnalytics(Result);
	return Result;
}

TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> FHaivenAnalytics::Create(
	Haiven::RestTypes::AnalyticsContext Context,
	Haiven::FAnalyticsBatcherOptions Options,
	FSend Send,
	FSchedule Schedule,
	FPost Post,
	FDiagnosticSink DiagnosticSink,
	FString AnalyticsSessionId,
	TFunction<FString()> NextEventId,
	TFunction<FString()> UtcNow)
{
	if (AnalyticsSessionId.IsEmpty()) AnalyticsSessionId = HaivenAnalyticsDetail::NewId();
	if (!NextEventId) NextEventId = [] { return HaivenAnalyticsDetail::NewId(); };
	if (!UtcNow) UtcNow = [] { return FDateTime::UtcNow().ToIso8601(); };
	TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Result = MakeShareable(new FHaivenAnalytics(
		std::move(Context), Options, MoveTemp(Send), MoveTemp(Schedule), MoveTemp(Post), MoveTemp(DiagnosticSink),
		MoveTemp(AnalyticsSessionId), MoveTemp(NextEventId), MoveTemp(UtcNow)));
	Result->Start();
	return Result;
}

FHaivenAnalytics::FHaivenAnalytics(
	Haiven::RestTypes::AnalyticsContext Context,
	Haiven::FAnalyticsBatcherOptions Options,
	FSend InSend,
	FSchedule InSchedule,
	FPost InPost,
	FDiagnosticSink InDiagnosticSink,
	FString AnalyticsSessionId,
	TFunction<FString()> NextEventId,
	TFunction<FString()> UtcNow)
	: Send(MoveTemp(InSend))
	, Schedule(MoveTemp(InSchedule))
	, Post(MoveTemp(InPost))
	, DiagnosticSink(MoveTemp(InDiagnosticSink))
{
	if (!IsInGameThread())
		ConfigurationError = TEXT("haiven: Unreal analytics must be created on the game thread");
	else if (!Send || !Schedule || !Post)
		ConfigurationError = TEXT("haiven: Unreal analytics host adapters are required");

	Core = MakeUnique<Haiven::FAnalyticsBatcherCore>(
		std::move(Context),
		Options,
		HaivenAnalyticsDetail::ToUtf8(AnalyticsSessionId),
		[NextEventId = MoveTemp(NextEventId)] { return HaivenAnalyticsDetail::ToUtf8(NextEventId()); },
		[UtcNow = MoveTemp(UtcNow)] { return HaivenAnalyticsDetail::ToUtf8(UtcNow()); });
	if (ConfigurationError.IsEmpty() && !Core->IsValid())
		ConfigurationError = HaivenAnalyticsDetail::FromUtf8(Core->GetConfigurationError());
}

void FHaivenAnalytics::Start()
{
	if (ConfigurationError.IsEmpty() || !Post || !DiagnosticSink) return;
	Haiven::FAnalyticsDiagnostic Diagnostic;
	Diagnostic.Kind = Haiven::EAnalyticsDiagnosticKind::EventRejected;
	Diagnostic.Reason = HaivenAnalyticsDetail::ToUtf8(ConfigurationError);
	Publish(Diagnostic);
}

bool FHaivenAnalytics::RejectWrongThread(const TCHAR* Operation)
{
	if (IsInGameThread()) return false;
	Haiven::FAnalyticsDiagnostic Diagnostic;
	Diagnostic.Kind = Haiven::EAnalyticsDiagnosticKind::EventRejected;
	Diagnostic.Reason = HaivenAnalyticsDetail::ToUtf8(
		FString::Printf(TEXT("haiven: %s must be called on the game thread"), Operation));
	Publish(Diagnostic);
	return true;
}

bool FHaivenAnalytics::TryEnqueue(const FString& Type, const FString& PropertiesJson, const FString& OccurredAt)
{
	if (RejectWrongThread(TEXT("analytics enqueue")) || bShutdown || !Core || !ConfigurationError.IsEmpty()) return false;
	Haiven::RestTypes::AnalyticsEvent Event;
	Event.Type = HaivenAnalyticsDetail::ToUtf8(Type);
	Event.Properties = HaivenAnalyticsDetail::ToUtf8(PropertiesJson);
	if (!OccurredAt.IsEmpty()) Event.OccurredAt = HaivenAnalyticsDetail::ToUtf8(OccurredAt);
	std::optional<Haiven::FAnalyticsDiagnostic> Diagnostic;
	const bool bAccepted = Core->TryEnqueue(std::move(Event), Diagnostic);
	if (Diagnostic) Publish(*Diagnostic);
	if (bAccepted && !bSending && !bPumpScheduled)
		SchedulePump(static_cast<double>(Core->GetOptions().FlushIntervalMilliseconds) / 1000.0);
	return bAccepted;
}

void FHaivenAnalytics::Flush(FFlushComplete Completion)
{
	if (!IsInGameThread())
	{
		if (Post)
		{
			const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Self = AsShared();
			Post([Self, Completion = MoveTemp(Completion)]() mutable { Self->Flush(MoveTemp(Completion)); });
		}
		else if (Completion)
		{
			Completion();
		}
		return;
	}
	if (Completion) PendingFlushes.Add(MoveTemp(Completion));
	if (!Core || !ConfigurationError.IsEmpty() || bShutdown || Core->IsIdle())
	{
		CompleteFlushesIfIdle();
		return;
	}
	if (!bSending && !bWaitingForRetry) SchedulePump(0.0, true);
}

void FHaivenAnalytics::SchedulePump(double DelaySeconds, bool bForce)
{
	if (bShutdown || !Schedule || (bPumpScheduled && !bForce)) return;
	bPumpScheduled = true;
	const uint64 Generation = ++PumpGeneration;
	TWeakPtr<FHaivenAnalytics, ESPMode::ThreadSafe> WeakThis = AsShared();
	Schedule(DelaySeconds, [WeakThis, Generation]
	{
		if (const TSharedPtr<FHaivenAnalytics, ESPMode::ThreadSafe> Self = WeakThis.Pin()) Self->Pump(Generation);
	});
}

void FHaivenAnalytics::Pump(uint64 Generation)
{
	if (!IsInGameThread())
	{
		if (Post)
		{
			TWeakPtr<FHaivenAnalytics, ESPMode::ThreadSafe> WeakThis = AsShared();
			Post([WeakThis, Generation]
			{
				if (const TSharedPtr<FHaivenAnalytics, ESPMode::ThreadSafe> Self = WeakThis.Pin()) Self->Pump(Generation);
			});
		}
		return;
	}
	if (Generation != PumpGeneration) return;
	bPumpScheduled = false;
	bWaitingForRetry = false;
	if (bShutdown || bSending || !Core || !ConfigurationError.IsEmpty())
	{
		CompleteFlushesIfIdle();
		return;
	}
	const std::optional<Haiven::Rest::FRestRequest> Request = Core->BeginBatch();
	if (!Request)
	{
		CompleteFlushesIfIdle();
		return;
	}
	bSending = true;
	FHaivenRestRequest UnrealRequest;
	UnrealRequest.Method = HaivenAnalyticsDetail::FromUtf8(Request->Method);
	UnrealRequest.Path = HaivenAnalyticsDetail::FromUtf8(Request->Path);
	UnrealRequest.Body = HaivenAnalyticsDetail::FromUtf8(Request->Body);
	UnrealRequest.ContentType = HaivenAnalyticsDetail::FromUtf8(Request->ContentType);
	TWeakPtr<FHaivenAnalytics, ESPMode::ThreadSafe> WeakThis = AsShared();
	Send(UnrealRequest, [WeakThis](const FHaivenHttpResult& Reply)
	{
		if (const TSharedPtr<FHaivenAnalytics, ESPMode::ThreadSafe> Self = WeakThis.Pin()) Self->HandleReply(Reply);
	});
}

void FHaivenAnalytics::HandleReply(const FHaivenHttpResult& Reply)
{
	if (!IsInGameThread())
	{
		if (Post)
		{
			TWeakPtr<FHaivenAnalytics, ESPMode::ThreadSafe> WeakThis = AsShared();
			Post([WeakThis, Reply]
			{
				if (const TSharedPtr<FHaivenAnalytics, ESPMode::ThreadSafe> Self = WeakThis.Pin()) Self->HandleReply(Reply);
			});
		}
		return;
	}
	if (bShutdown || !Core) return;
	bSending = false;
	const Haiven::FAnalyticsDispatchResult Result = Core->HandleReply(
		Reply.StatusCode,
		HaivenAnalyticsDetail::ToUtf8(Reply.Body),
		HaivenAnalyticsDetail::ToUtf8(Reply.Error));
	Publish(Result.Diagnostics);
	if (Result.Action == Haiven::FAnalyticsDispatchResult::EAction::Retry)
	{
		bWaitingForRetry = true;
		SchedulePump(static_cast<double>(Result.RetryAfterMilliseconds) / 1000.0, true);
	}
	else if (Core->BufferedEventCount() > 0)
	{
		SchedulePump(0.0, true);
	}
	else
	{
		CompleteFlushesIfIdle();
	}
}

void FHaivenAnalytics::CompleteFlushesIfIdle()
{
	if (Core && !Core->IsIdle()) return;
	TArray<FFlushComplete> Completions = MoveTemp(PendingFlushes);
	PendingFlushes.Reset();
	for (FFlushComplete& Completion : Completions)
	{
		if (!Completion) continue;
		if (Post)
			Post([Completion = MoveTemp(Completion)]() mutable { Completion(); });
		else
			Completion();
	}
}

void FHaivenAnalytics::Publish(const std::vector<Haiven::FAnalyticsDiagnostic>& Diagnostics)
{
	for (const Haiven::FAnalyticsDiagnostic& Diagnostic : Diagnostics) Publish(Diagnostic);
}

void FHaivenAnalytics::Publish(const Haiven::FAnalyticsDiagnostic& Diagnostic)
{
	if (!DiagnosticSink || !Post) return;
	TWeakPtr<FHaivenAnalytics, ESPMode::ThreadSafe> WeakThis = AsShared();
	Post([WeakThis, Diagnostic]
	{
		if (const TSharedPtr<FHaivenAnalytics, ESPMode::ThreadSafe> Self = WeakThis.Pin())
		{
			if (Self->DiagnosticSink) Self->DiagnosticSink(Diagnostic);
		}
	});
}

void FHaivenAnalytics::Shutdown()
{
	if (!IsInGameThread())
	{
		if (Post)
		{
			const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Self = AsShared();
			Post([Self] { Self->Shutdown(); });
		}
		return;
	}
	if (bShutdown) return;
	bShutdown = true;
	++PumpGeneration;
	if (Core) Publish(Core->Dispose());
	bSending = false;
	bPumpScheduled = false;
	bWaitingForRetry = false;
	CompleteFlushesIfIdle();
}

bool FHaivenAnalytics::IsValid(FString* Error) const
{
	if (!IsInGameThread())
	{
		if (Error) *Error = TEXT("haiven: analytics validity must be queried on the game thread");
		return false;
	}
	if (Error) *Error = ConfigurationError;
	return ConfigurationError.IsEmpty() && Core && Core->IsValid();
}

int32 FHaivenAnalytics::GetBufferedEventCount() const
{
	return IsInGameThread() && Core ? static_cast<int32>(Core->BufferedEventCount()) : 0;
}

uint64 FHaivenAnalytics::GetDroppedEventCount() const
{
	return IsInGameThread() && Core ? Core->DroppedEventCount() : 0;
}

uint64 FHaivenAnalytics::GetRetryCount() const
{
	return IsInGameThread() && Core ? Core->RetryCount() : 0;
}
