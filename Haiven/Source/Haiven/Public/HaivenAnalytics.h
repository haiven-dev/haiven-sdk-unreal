#pragma once

#include "CoreMinimal.h"
#include "HaivenAnalyticsCore.h"
#include "HaivenClientTypes.h"

class UHaivenSubsystem;

/**
 * Unreal's non-blocking adapter around the engine-free analytics batcher.
 * CreateForSubsystem uses the core ticker, authenticated subsystem HTTP, and
 * game-thread posting. Public methods are game-thread-only; release builds reject
 * or marshal calls made from another thread instead of mutating adapter state.
 * TryEnqueue never waits for a timer or network request.
 */
class HAIVEN_API FHaivenAnalytics : public TSharedFromThis<FHaivenAnalytics, ESPMode::ThreadSafe>
{
public:
	using FComplete = TFunction<void(const FHaivenHttpResult&)>;
	using FSend = TFunction<void(const FHaivenRestRequest&, FComplete)>;
	using FSchedule = TFunction<void(double, TFunction<void()>)>;
	using FPost = TFunction<void(TFunction<void()>)>;
	using FDiagnosticSink = TFunction<void(const Haiven::FAnalyticsDiagnostic&)>;
	using FFlushComplete = TFunction<void()>;

	/**
	 * A ready-to-run bridge using UHaivenSubsystem::Request and Unreal's core ticker.
	 * The owner shuts this instance down on every credential replacement; create a
	 * new instance after switching accounts.
	 */
	static TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> CreateForSubsystem(
		UHaivenSubsystem* Owner,
		Haiven::RestTypes::AnalyticsContext Context,
		Haiven::FAnalyticsBatcherOptions Options = {},
		FDiagnosticSink DiagnosticSink = {});

	/** Injectable host seam used by automation tests and unusual engine hosts. */
	static TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Create(
		Haiven::RestTypes::AnalyticsContext Context,
		Haiven::FAnalyticsBatcherOptions Options,
		FSend Send,
		FSchedule Schedule,
		FPost Post,
		FDiagnosticSink DiagnosticSink,
		FString AnalyticsSessionId = FString(),
		TFunction<FString()> NextEventId = {},
		TFunction<FString()> UtcNow = {});

	bool TryEnqueue(
		const FString& Type,
		const FString& PropertiesJson = TEXT("{}"),
		const FString& OccurredAt = FString());

	/** Completes asynchronously after all accepted events are delivered or dropped. */
	void Flush(FFlushComplete Completion = {});
	void Shutdown();
	bool IsValid(FString* Error = nullptr) const;
	int32 GetBufferedEventCount() const;
	uint64 GetDroppedEventCount() const;
	uint64 GetRetryCount() const;

private:
	FHaivenAnalytics(
		Haiven::RestTypes::AnalyticsContext Context,
		Haiven::FAnalyticsBatcherOptions Options,
		FSend InSend,
		FSchedule InSchedule,
		FPost InPost,
		FDiagnosticSink InDiagnosticSink,
		FString AnalyticsSessionId,
		TFunction<FString()> NextEventId,
		TFunction<FString()> UtcNow);

	void Start();
	void SchedulePump(double DelaySeconds, bool bForce = false);
	void Pump(uint64 Generation);
	void HandleReply(const FHaivenHttpResult& Reply);
	void CompleteFlushesIfIdle();
	bool RejectWrongThread(const TCHAR* Operation);
	void Publish(const std::vector<Haiven::FAnalyticsDiagnostic>& Diagnostics);
	void Publish(const Haiven::FAnalyticsDiagnostic& Diagnostic);

	TUniquePtr<Haiven::FAnalyticsBatcherCore> Core;
	FSend Send;
	FSchedule Schedule;
	FPost Post;
	FDiagnosticSink DiagnosticSink;
	TArray<FFlushComplete> PendingFlushes;
	FString ConfigurationError;
	uint64 PumpGeneration = 0;
	bool bPumpScheduled = false;
	bool bSending = false;
	bool bWaitingForRetry = false;
	bool bShutdown = false;
};
