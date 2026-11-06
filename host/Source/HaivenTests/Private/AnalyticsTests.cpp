#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "HaivenAnalytics.h"
#include "HaivenHandoff.h"
#include "HaivenSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenAnalyticsBridge,
	"Haiven.Analytics.Bridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	Haiven::RestTypes::AnalyticsContext AnalyticsContext()
	{
		Haiven::RestTypes::AnalyticsContext Context;
		Context.Environment = "development";
		Context.IntegrationId = "game-1";
		Context.Platform = "unreal";
		return Context;
	}

	void Drain(TArray<TFunction<void()>>& Callbacks)
	{
		while (!Callbacks.IsEmpty())
		{
			TFunction<void()> Callback = MoveTemp(Callbacks[0]);
			Callbacks.RemoveAt(0);
			Callback();
		}
	}
}

bool FHaivenAnalyticsBridge::RunTest(const FString& Parameters)
{
	Haiven::FAnalyticsBatcherOptions Options;
	Options.BatchSize = 2;
	Options.MaxBufferedEvents = 2;
	Options.MaxRetries = 1;
	Options.FlushIntervalMilliseconds = 25;
	Options.InitialRetryDelayMilliseconds = 50;
	Options.MaxRetryDelayMilliseconds = 50;

	TArray<double> Delays;
	TArray<TFunction<void()>> Scheduled;
	TArray<TFunction<void()>> Posted;
	TArray<FHaivenRestRequest> Requests;
	FHaivenAnalytics::FComplete PendingReply;
	TArray<Haiven::FAnalyticsDiagnostic> Diagnostics;
	int32 NextId = 0;
	int32 Flushes = 0;
	const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Analytics = FHaivenAnalytics::Create(
		AnalyticsContext(),
		Options,
		[&Requests, &PendingReply](const FHaivenRestRequest& Request, FHaivenAnalytics::FComplete Complete)
		{
			Requests.Add(Request);
			PendingReply = MoveTemp(Complete);
		},
		[&Delays, &Scheduled](double Delay, TFunction<void()> Callback)
		{
			Delays.Add(Delay);
			Scheduled.Add(MoveTemp(Callback));
		},
		[&Posted](TFunction<void()> Callback) { Posted.Add(MoveTemp(Callback)); },
		[&Diagnostics](const Haiven::FAnalyticsDiagnostic& Diagnostic) { Diagnostics.Add(Diagnostic); },
		TEXT("session-1"),
		[&NextId] { return FString::Printf(TEXT("event-%d"), ++NextId); },
		[] { return TEXT("2026-10-14T12:00:00.000Z"); });

	FString ConfigurationError;
	TestTrue(TEXT("the injected adapter is valid on the game thread"), Analytics->IsValid(&ConfigurationError));
	TestTrue(TEXT("valid configuration has no error"), ConfigurationError.IsEmpty());
	TestFalse(TEXT("malformed properties are rejected"), Analytics->TryEnqueue(TEXT("sdk.health"), TEXT("not-json")));
	TestFalse(TEXT("nested properties are rejected"), Analytics->TryEnqueue(TEXT("sdk.health"), TEXT(R"({"nested":{"value":1}})")));
	TestTrue(TEXT("invalid-input diagnostics are deferred"), Diagnostics.IsEmpty() && Posted.Num() == 2);
	Drain(Posted);
	TestTrue(TEXT("invalid properties are diagnosed"), Diagnostics.Num() == 2
		&& Diagnostics[0].Kind == Haiven::EAnalyticsDiagnosticKind::EventRejected
		&& Diagnostics[1].Kind == Haiven::EAnalyticsDiagnosticKind::EventRejected);

	TestTrue(TEXT("enqueue returns before scheduling runs"),
		Analytics->TryEnqueue(TEXT("sdk.health"), TEXT(R"({"status":"ok","sampled":true})")));
	TestTrue(TEXT("the memory bound accepts a second event"),
		Analytics->TryEnqueue(TEXT("game.launch"), TEXT(R"({"source":"menu"})")));
	TestFalse(TEXT("and rejects overflow without sending synchronously"),
		Analytics->TryEnqueue(TEXT("session.end"), TEXT(R"({"reason":"overflow"})")));
	TestTrue(TEXT("one deferred window exists before flush"), Scheduled.Num() == 1 && Delays[0] == 0.025 && Requests.IsEmpty());
	Analytics->Flush([&Flushes] { ++Flushes; });
	TestTrue(TEXT("flush supersedes the window with an immediate pump"), Scheduled.Num() == 2 && Delays[1] == 0.0);
	Scheduled[1]();
	TestTrue(TEXT("the generated endpoint is sent by the injected bridge"), Requests.Num() == 1
		&& Requests[0].Method == TEXT("POST") && Requests[0].Path == TEXT("/sdk/analytics/events"));
	TestTrue(TEXT("the batch carries stable ids, session, time and flat properties"),
		Requests[0].Body.Contains(TEXT("\"eventId\":\"event-1\""))
		&& Requests[0].Body.Contains(TEXT("\"analyticsSessionId\":\"session-1\""))
		&& Requests[0].Body.Contains(TEXT("\"occurredAt\":\"2026-10-14T12:00:00.000Z\""))
		&& Requests[0].Body.Contains(TEXT("\"status\":\"ok\"")));
	const FString FirstBody = Requests[0].Body;

	FHaivenHttpResult EmptySuccess;
	EmptySuccess.bSucceeded = true;
	EmptySuccess.StatusCode = 202;
	PendingReply(EmptySuccess);
	TestTrue(TEXT("empty 2xx is malformed and retryable"), Scheduled.Num() == 3
		&& Delays[2] == 0.05 && Analytics->GetRetryCount() == 1 && Flushes == 0);
	Scheduled[2]();
	TestTrue(TEXT("retry sends the exact immutable payload"), Requests.Num() == 2 && Requests[1].Body == FirstBody);

	FHaivenHttpResult Accepted;
	Accepted.bSucceeded = true;
	Accepted.StatusCode = 202;
	Accepted.Body = TEXT(R"({"accepted":2,"duplicate":0,"rejected":[]})");
	PendingReply(Accepted);
	Drain(Posted);
	TestEqual(TEXT("flush completes only after the batch becomes idle"), Flushes, 1);
	TestEqual(TEXT("delivered events leave the buffer"), Analytics->GetBufferedEventCount(), 0);

	const int32 RequestsBeforeScheduledShutdown = Requests.Num();
	TestTrue(TEXT("an event can be queued before scheduled shutdown"),
		Analytics->TryEnqueue(TEXT("session.end"), TEXT(R"({"reason":"quit"})")));
	const int32 ScheduledShutdownIndex = Scheduled.Num() - 1;
	Analytics->Shutdown();
	Scheduled[ScheduledShutdownIndex]();
	Drain(Posted);
	TestEqual(TEXT("scheduled shutdown clears without sending"), Requests.Num(), RequestsBeforeScheduledShutdown);
	TestEqual(TEXT("scheduled shutdown drops retained events"), Analytics->GetBufferedEventCount(), 0);
	TestFalse(TEXT("shutdown refuses later gameplay events"), Analytics->TryEnqueue(TEXT("sdk.health")));

	{
		TArray<TFunction<void()>> InFlightScheduled;
		TArray<TFunction<void()>> InFlightPosted;
		FHaivenAnalytics::FComplete InFlightReply;
		int32 InFlightSends = 0;
		int32 InFlightFlushes = 0;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> InFlight = FHaivenAnalytics::Create(
			AnalyticsContext(), Options,
			[&InFlightReply, &InFlightSends](const FHaivenRestRequest&, FHaivenAnalytics::FComplete Complete)
			{
				++InFlightSends;
				InFlightReply = MoveTemp(Complete);
			},
			[&InFlightScheduled](double, TFunction<void()> Callback) { InFlightScheduled.Add(MoveTemp(Callback)); },
			[&InFlightPosted](TFunction<void()> Callback) { InFlightPosted.Add(MoveTemp(Callback)); },
			{}, TEXT("session-2"), [] { return TEXT("in-flight-event"); },
			[] { return TEXT("2026-10-14T12:00:00.000Z"); });
		TestTrue(TEXT("in-flight shutdown setup enqueues"), InFlight->TryEnqueue(TEXT("session.end")));
		InFlight->Flush([&InFlightFlushes] { ++InFlightFlushes; });
		InFlightScheduled.Last()();
		TestEqual(TEXT("in-flight shutdown setup sent once"), InFlightSends, 1);
		InFlight->Shutdown();
		Drain(InFlightPosted);
		TestEqual(TEXT("shutdown completes a pending drain after dropping in-flight work"), InFlightFlushes, 1);
		FHaivenHttpResult LateReply;
		LateReply.bSucceeded = true;
		LateReply.StatusCode = 202;
		LateReply.Body = TEXT("{}");
		InFlightReply(LateReply);
		TestEqual(TEXT("late in-flight completion is ignored after shutdown"), InFlight->GetBufferedEventCount(), 0);
	}

	{
		TArray<TFunction<void()>> RetryScheduled;
		TArray<TFunction<void()>> RetryPosted;
		FHaivenAnalytics::FComplete RetryReply;
		TArray<Haiven::FAnalyticsDiagnostic> RetryDiagnostics;
		int32 RetryFlushes = 0;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Exhaustion = FHaivenAnalytics::Create(
			AnalyticsContext(), Options,
			[&RetryReply](const FHaivenRestRequest&, FHaivenAnalytics::FComplete Complete) { RetryReply = MoveTemp(Complete); },
			[&RetryScheduled](double, TFunction<void()> Callback) { RetryScheduled.Add(MoveTemp(Callback)); },
			[&RetryPosted](TFunction<void()> Callback) { RetryPosted.Add(MoveTemp(Callback)); },
			[&RetryDiagnostics](const Haiven::FAnalyticsDiagnostic& Diagnostic) { RetryDiagnostics.Add(Diagnostic); },
			TEXT("session-3"), [] { return TEXT("retry-event"); },
			[] { return TEXT("2026-10-14T12:00:00.000Z"); });
		TestTrue(TEXT("retry exhaustion setup enqueues"), Exhaustion->TryEnqueue(TEXT("sdk.health")));
		Exhaustion->Flush([&RetryFlushes] { ++RetryFlushes; });
		RetryScheduled.Last()();
		FHaivenHttpResult Unavailable;
		Unavailable.StatusCode = 503;
		RetryReply(Unavailable);
		RetryScheduled.Last()();
		RetryReply(Unavailable);
		Drain(RetryPosted);
		TestEqual(TEXT("retry exhaustion drops and completes flush"), RetryFlushes, 1);
		TestEqual(TEXT("retry exhaustion leaves no buffered events"), Exhaustion->GetBufferedEventCount(), 0);
		TestTrue(TEXT("retry exhaustion emits a capped drop diagnostic"), RetryDiagnostics.Num() == 2
			&& RetryDiagnostics.Last().Kind == Haiven::EAnalyticsDiagnosticKind::BatchDropped
			&& FString(UTF8_TO_TCHAR(RetryDiagnostics.Last().Reason.c_str())).Contains(TEXT("retry limit reached")));
		Exhaustion->Shutdown();
	}

	{
		Haiven::FAnalyticsBatcherOptions InvalidOptions = Options;
		InvalidOptions.BatchSize = 101;
		TArray<TFunction<void()>> InvalidPosted;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> Invalid = FHaivenAnalytics::Create(
			AnalyticsContext(), InvalidOptions,
			[](const FHaivenRestRequest&, FHaivenAnalytics::FComplete) {},
			[](double, TFunction<void()>) {},
			[&InvalidPosted](TFunction<void()> Callback) { InvalidPosted.Add(MoveTemp(Callback)); },
			{}, TEXT("session-invalid"), [] { return TEXT("invalid-event"); }, [] { return TEXT("now"); });
		FString InvalidError;
		TestFalse(TEXT("invalid configuration is finite and inspectable"), Invalid->IsValid(&InvalidError));
		TestTrue(TEXT("invalid configuration records its reason"), InvalidError.Contains(TEXT("batch size")));
		TestFalse(TEXT("invalid configuration rejects enqueue without throwing"), Invalid->TryEnqueue(TEXT("sdk.health")));
		Invalid->Shutdown();
	}

	// Subsystem-backed adapters are account scoped. Exercise every credential
	// transition with work in each state that could otherwise inherit a new bearer.
	{
		UHaivenSubsystem* Owner = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
		Owner->SetAccessToken(TEXT("player-a"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> ProductBridge =
			FHaivenAnalytics::CreateForSubsystem(Owner, AnalyticsContext(), Options);
		TestTrue(TEXT("CreateForSubsystem binds its exact bridge to the account owner"),
			Owner->AnalyticsInstances.Num() == 1
			&& Owner->AnalyticsInstances[0].Pin().Get() == &ProductBridge.Get());
		TestTrue(TEXT("the product bridge queues player A work"), ProductBridge->TryEnqueue(TEXT("sdk.health")));
		Owner->SetAccessToken(TEXT("player-b"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		TestTrue(TEXT("the production registration path drops queued player A work on token swap"),
			ProductBridge->GetBufferedEventCount() == 0 && !ProductBridge->TryEnqueue(TEXT("sdk.health"))
			&& Owner->AnalyticsInstances.IsEmpty());

		Owner->SetAccessToken(TEXT("player-a"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		TArray<TFunction<void()>> BoundaryScheduled;
		int32 BoundarySends = 0;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> PlayerA = FHaivenAnalytics::Create(
			AnalyticsContext(), Options,
			[&BoundarySends](const FHaivenRestRequest&, FHaivenAnalytics::FComplete) { ++BoundarySends; },
			[&BoundaryScheduled](double, TFunction<void()> Callback) { BoundaryScheduled.Add(MoveTemp(Callback)); },
			[](TFunction<void()> Callback) { Callback(); }, {}, TEXT("account-a-queued"),
			[] { return TEXT("account-a-queued-event"); }, [] { return TEXT("2026-10-14T12:00:00.000Z"); });
		Owner->RegisterAnalytics(PlayerA);
		TestTrue(TEXT("player A analytics can queue before a token swap"), PlayerA->TryEnqueue(TEXT("sdk.health")));
		Owner->SetAccessToken(TEXT("player-b"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		BoundaryScheduled.Last()();
		TestEqual(TEXT("a queued player A event never sends under player B"), BoundarySends, 0);
		TestEqual(TEXT("the token swap drops the queued player A event"), PlayerA->GetBufferedEventCount(), 0);
		TestFalse(TEXT("the account-scoped player A adapter stays closed"), PlayerA->TryEnqueue(TEXT("sdk.health")));

		TArray<TFunction<void()>> PlayerBScheduled;
		FHaivenAnalytics::FComplete PlayerBReply;
		int32 PlayerBSends = 0;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> PlayerB = FHaivenAnalytics::Create(
			AnalyticsContext(), Options,
			[&PlayerBSends, &PlayerBReply](const FHaivenRestRequest&, FHaivenAnalytics::FComplete Complete)
			{
				++PlayerBSends;
				PlayerBReply = MoveTemp(Complete);
			},
			[&PlayerBScheduled](double, TFunction<void()> Callback) { PlayerBScheduled.Add(MoveTemp(Callback)); },
			[](TFunction<void()> Callback) { Callback(); }, {}, TEXT("account-b"),
			[] { return TEXT("account-b-event"); }, [] { return TEXT("2026-10-14T12:00:00.000Z"); });
		Owner->RegisterAnalytics(PlayerB);
		TestTrue(TEXT("player B recreates analytics after the boundary"), PlayerB->TryEnqueue(TEXT("sdk.health")));
		PlayerB->Flush();
		PlayerBScheduled.Last()();
		TestEqual(TEXT("the fresh player B adapter can send"), PlayerBSends, 1);
		FHaivenHttpResult PlayerBAccepted;
		PlayerBAccepted.bSucceeded = true;
		PlayerBAccepted.StatusCode = 202;
		PlayerBAccepted.Body = TEXT(R"({"accepted":1,"duplicate":0,"rejected":[]})");
		PlayerBReply(PlayerBAccepted);
		Owner->Logout();
	}

	{
		UHaivenSubsystem* Owner = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
		Owner->SetAccessToken(TEXT("player-a"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		TArray<TFunction<void()>> BoundaryScheduled;
		FHaivenAnalytics::FComplete Pending;
		int32 BoundarySends = 0;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> PlayerA = FHaivenAnalytics::Create(
			AnalyticsContext(), Options,
			[&BoundarySends, &Pending](const FHaivenRestRequest&, FHaivenAnalytics::FComplete Complete)
			{
				++BoundarySends;
				Pending = MoveTemp(Complete);
			},
			[&BoundaryScheduled](double, TFunction<void()> Callback) { BoundaryScheduled.Add(MoveTemp(Callback)); },
			[](TFunction<void()> Callback) { Callback(); }, {}, TEXT("account-a-retry"),
			[] { return TEXT("account-a-retry-event"); }, [] { return TEXT("2026-10-14T12:00:00.000Z"); });
		Owner->RegisterAnalytics(PlayerA);
		PlayerA->TryEnqueue(TEXT("sdk.health"));
		PlayerA->Flush();
		BoundaryScheduled.Last()();
		FHaivenHttpResult Unavailable;
		Unavailable.StatusCode = 503;
		Pending(Unavailable);
		const int32 RetryIndex = BoundaryScheduled.Num() - 1;
		Owner->Logout();
		Owner->SetAccessToken(TEXT("player-b"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		BoundaryScheduled[RetryIndex]();
		TestEqual(TEXT("a waiting player A retry never sends under player B"), BoundarySends, 1);
		TestEqual(TEXT("logout drops the retry batch before player B signs in"), PlayerA->GetBufferedEventCount(), 0);
	}

	{
		UHaivenSubsystem* Owner = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
		Owner->Handoff = MakeUnique<Haiven::FHandoffCore>();
		Owner->SetAccessToken(TEXT("player-a"), FDateTime::UtcNow() + FTimespan::FromHours(1));
		TArray<TFunction<void()>> BoundaryScheduled;
		FHaivenAnalytics::FComplete Pending;
		int32 BoundarySends = 0;
		const TSharedRef<FHaivenAnalytics, ESPMode::ThreadSafe> PlayerA = FHaivenAnalytics::Create(
			AnalyticsContext(), Options,
			[&BoundarySends, &Pending](const FHaivenRestRequest&, FHaivenAnalytics::FComplete Complete)
			{
				++BoundarySends;
				Pending = MoveTemp(Complete);
			},
			[&BoundaryScheduled](double, TFunction<void()> Callback) { BoundaryScheduled.Add(MoveTemp(Callback)); },
			[](TFunction<void()> Callback) { Callback(); }, {}, TEXT("account-a-inflight"),
			[] { return TEXT("account-a-inflight-event"); }, [] { return TEXT("2026-10-14T12:00:00.000Z"); });
		Owner->RegisterAnalytics(PlayerA);
		PlayerA->TryEnqueue(TEXT("sdk.health"));
		PlayerA->Flush();
		BoundaryScheduled.Last()();
		TestEqual(TEXT("player A analytics is in flight before handoff replacement"), BoundarySends, 1);
		Owner->FinishExchange(true, 200,
			TEXT(R"({"access_token":"player-b","token_type":"Bearer","expires_in":3600})"), FString());
		FHaivenHttpResult LateUnavailable;
		LateUnavailable.StatusCode = 503;
		Pending(LateUnavailable);
		TestEqual(TEXT("a late player A completion cannot schedule a retry under handoff player B"),
			BoundaryScheduled.Num(), 2);
		TestEqual(TEXT("handoff replacement drops the in-flight player A batch"), PlayerA->GetBufferedEventCount(), 0);
		TestEqual(TEXT("the in-flight player A request remains the only send"), BoundarySends, 1);
		Owner->Logout();
	}

	return true;
}
#endif
