#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "HaivenFrames.h"
#include "HaivenSessionCore.h"
#include "HaivenSubsystem.h"

#include <string>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenRealtimeReconnect,
	"Haiven.Realtime.Reconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenRealtimeReconnect::RunTest(const FString& Parameters)
{
	// The backoff: doubling from the initial delay, capped, with equal jitter.
	TestEqual(TEXT("the first attempt waits the initial delay at most"),
		UHaivenSubsystem::RealtimeReconnectDelaySeconds(1, 1.0f, 30.0f, 1.0f), 1.0f);
	TestEqual(TEXT("jitter never makes a reconnect immediate"),
		UHaivenSubsystem::RealtimeReconnectDelaySeconds(1, 1.0f, 30.0f, 0.0f), 0.5f);
	TestEqual(TEXT("each attempt doubles the step"),
		UHaivenSubsystem::RealtimeReconnectDelaySeconds(4, 1.0f, 30.0f, 1.0f), 8.0f);
	TestEqual(TEXT("the step is capped"),
		UHaivenSubsystem::RealtimeReconnectDelaySeconds(6, 1.0f, 30.0f, 1.0f), 30.0f);
	TestEqual(TEXT("the cap holds for any attempt"),
		UHaivenSubsystem::RealtimeReconnectDelaySeconds(1000, 1.0f, 30.0f, 0.0f), 15.0f);
	TestEqual(TEXT("a zero initial delay is raised to 0.1 seconds"),
		UHaivenSubsystem::RealtimeReconnectDelaySeconds(1, 0.0f, 30.0f, 1.0f), 0.1f);

	// Each connect starts a real transport to a closed loopback port. The test
	// then reports a close for the current generation itself, synchronously, so
	// the transport's own later failure is for a generation already retired.
	UHaivenSubsystem* Subsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	Subsystem->ResetChat();
	Subsystem->ResetIdentity();
	FHaivenClientOptions Options;
	Options.BaseUrl = TEXT("http://127.0.0.1:9");
	Options.RealtimeUrl = TEXT("ws://127.0.0.1:9");
	Subsystem->Configure(Options);
	const FDateTime Expiry = FDateTime::UtcNow() + FTimespan::FromHours(1);
	Subsystem->SetAccessToken(TEXT("player-a"), Expiry);

	auto Connect = [Subsystem]()
	{
		Subsystem->ConnectRealtime();
		return Subsystem->RealtimeGeneration;
	};
	auto CloseBy = [Subsystem](int32 Code, const FString& Reason)
	{
		Subsystem->HandleSocketClosed(Subsystem->RealtimeGeneration, Code, Reason);
	};

	Connect();
	TestEqual(TEXT("connect opens a socket"), Subsystem->GetRealtimeState(), EHaivenRealtimeState::Connecting);
	CloseBy(1006, TEXT("network"));
	TestTrue(TEXT("an unexpected close schedules a reconnect"), Subsystem->IsRealtimeReconnectScheduled());
	TestEqual(TEXT("the socket is reported closed while it waits"),
		Subsystem->GetRealtimeState(), EHaivenRealtimeState::Disconnected);
	TestEqual(TEXT("it is the first attempt"), Subsystem->RealtimeReconnectAttempt, 1);
	TestTrue(TEXT("the first attempt waits within the initial step"),
		Subsystem->RealtimeReconnectDelay >= 0.5f && Subsystem->RealtimeReconnectDelay <= 1.0f);

	const uint64 Dropped = Subsystem->RealtimeGeneration;
	Subsystem->RunRealtimeReconnect();
	TestTrue(TEXT("the reconnect opens a new socket"),
		Subsystem->RealtimeGeneration > Dropped && Subsystem->RealtimeSession.IsValid());
	TestEqual(TEXT("the reconnect is connecting"), Subsystem->GetRealtimeState(), EHaivenRealtimeState::Connecting);
	TestFalse(TEXT("nothing else is scheduled while it connects"), Subsystem->IsRealtimeReconnectScheduled());
	Subsystem->HandleSocketError(Subsystem->RealtimeGeneration, TEXT("connection refused"));
	TestEqual(TEXT("a failed reconnect backs off further"), Subsystem->RealtimeReconnectAttempt, 2);
	TestTrue(TEXT("the second attempt waits within the doubled step"),
		Subsystem->RealtimeReconnectDelay >= 1.0f && Subsystem->RealtimeReconnectDelay <= 2.0f);

	Subsystem->RunRealtimeReconnect();
	Subsystem->RealtimeSession->OnStateChanged(Haiven::ESessionState::Ready);
	TestEqual(TEXT("an authenticated reconnect resets the backoff"), Subsystem->RealtimeReconnectAttempt, 0);
	CloseBy(1000, FString());
	TestTrue(TEXT("a server-initiated normal close is also reconnected"), Subsystem->IsRealtimeReconnectScheduled());
	TestEqual(TEXT("and starts the backoff over"), Subsystem->RealtimeReconnectAttempt, 1);

	Subsystem->DisconnectRealtime();
	TestFalse(TEXT("DisconnectRealtime cancels a scheduled reconnect"), Subsystem->IsRealtimeReconnectScheduled());
	TestEqual(TEXT("DisconnectRealtime restarts the backoff"), Subsystem->RealtimeReconnectAttempt, 0);

	Connect();
	CloseBy(1006, TEXT("network"));
	Connect();
	TestFalse(TEXT("the game's own connect supersedes a scheduled reconnect"), Subsystem->IsRealtimeReconnectScheduled());
	TestEqual(TEXT("the game's own connect restarts the backoff"), Subsystem->RealtimeReconnectAttempt, 0);

	CloseBy(1006, TEXT("network"));
	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
	TestFalse(TEXT("an account change cancels a scheduled reconnect"), Subsystem->IsRealtimeReconnectScheduled());

	Connect();
	CloseBy(1006, TEXT("network"));
	Subsystem->Logout();
	TestFalse(TEXT("Logout cancels a scheduled reconnect"), Subsystem->IsRealtimeReconnectScheduled());
	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);

	Connect();
	Subsystem->HostAccessTokenExpiry = FDateTime::UtcNow() - FTimespan::FromSeconds(1);
	CloseBy(1006, TEXT("network"));
	TestFalse(TEXT("without an unexpired token nothing reconnects"), Subsystem->IsRealtimeReconnectScheduled());

	for (const TCHAR* Reason : {TEXT("credential_revoked"), TEXT("parental_consent_denied"), TEXT("unsupported subprotocol")})
	{
		// Some policy closes intentionally clear the rejected credential. Give each
		// case an independent account so one policy cannot invalidate the next.
		Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
		Connect();
		CloseBy(1008, Reason);
		TestFalse(FString::Printf(TEXT("a policy close for the account (%s) is final"), Reason),
			Subsystem->IsRealtimeReconnectScheduled());
	}
	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
	Connect();
	CloseBy(1008, TEXT("unauthorized"));
	TestFalse(TEXT("an initial unauthorized close clears the credential instead of reconnecting"),
		Subsystem->IsRealtimeReconnectScheduled());

	// A live auth.failure answers the same-socket re-auth of an expiry episode.
	// The session is bound through the subsystem's real wiring, so the session
	// core raises the credential action first and reports the failure second,
	// exactly as it does on a live socket. Its writes go nowhere.
	const std::string AuthenticatedJson = R"({"type":"auth.success","requestId":"","success":true})";
	const std::string ExpiredJson = R"({"type":"auth.expired","requestId":"","success":false})";
	Haiven::FServerFrame Authenticated;
	Haiven::FServerFrame Expired;
	TestTrue(TEXT("the auth-success frame parses"), Haiven::ParseServerFrame(AuthenticatedJson, Authenticated));
	TestTrue(TEXT("the auth-expired frame parses"), Haiven::ParseServerFrame(ExpiredJson, Expired));
	int32 RefusalRequestIds = 0;
	Haiven::FSessionCore* RefusingSession = nullptr;
	auto RefuseLiveReauth = [this, Subsystem, Expiry, &Authenticated, &AuthenticatedJson, &Expired, &ExpiredJson,
		&RefusalRequestIds, &RefusingSession](const std::string& Code)
	{
		Subsystem->SetAccessToken(TEXT("player-b"), Expiry);
		Subsystem->DisconnectRealtime();
		Subsystem->RealtimeSession = MakeUnique<Haiven::FSessionCore>(
			[](const Haiven::FClientFrame&) {},
			[] { return std::string("player-b"); },
			[&RefusalRequestIds] { return "refusal-" + std::to_string(++RefusalRequestIds); });
		RefusingSession = Subsystem->RealtimeSession.Get();
		++Subsystem->RealtimeGeneration;
		Subsystem->BindRealtimeSession();
		const uint64 RefusingGeneration = Subsystem->RealtimeGeneration;
		RefusingSession->Opened();
		RefusingSession->Receive(Authenticated, AuthenticatedJson);
		RefusingSession->Receive(Expired, ExpiredJson);
		const std::string RefusedJson =
			R"({"type":"auth.failure","requestId":"","success":false,"error":")" + Code + R"("})";
		Haiven::FServerFrame Refused;
		TestTrue(TEXT("the auth-failure frame parses"), Haiven::ParseServerFrame(RefusedJson, Refused));
		RefusingSession->Receive(Refused, RefusedJson);
		return RefusingGeneration;
	};

	// Any other code keeps the credential, and this socket will not authenticate
	// again, so the subsystem does not reconnect it by itself.
	uint64 Refusing = RefuseLiveReauth("rate_limited");
	TestTrue(TEXT("a refusal with no fresh-socket retry keeps the refusing session"),
		Subsystem->RealtimeSession.Get() == RefusingSession);
	TestTrue(TEXT("and opens no new socket"), Subsystem->RealtimeGeneration == Refusing);
	CloseBy(1006, TEXT("network"));
	TestFalse(TEXT("a refused credential is not reconnected"), Subsystem->IsRealtimeReconnectScheduled());

	// ADR 0041: invalid_token and unauthorized retry the credential once as the
	// first frame of a fresh socket. The retry connects inside the credential
	// action, before the refusing session reports the failure. That late report
	// must not stop the fresh session from reconnecting with the kept credential.
	for (const char* Code : {"invalid_token", "unauthorized"})
	{
		const FString Label = UTF8_TO_TCHAR(Code);
		Refusing = RefuseLiveReauth(Code);
		TestTrue(FString::Printf(TEXT("%s retries on a fresh socket"), *Label),
			Subsystem->RealtimeGeneration > Refusing && Subsystem->RealtimeSession.IsValid()
				&& Subsystem->RealtimeSession.Get() != RefusingSession);
		TestTrue(FString::Printf(TEXT("%s retry is connecting"), *Label),
			Subsystem->GetRealtimeState() == EHaivenRealtimeState::Connecting);
		TestFalse(FString::Printf(TEXT("the refusing session's late %s report leaves the retry reconnectable"), *Label),
			Subsystem->bRealtimeAuthRefused);
		CloseBy(1013, TEXT("auth_unavailable"));
		TestTrue(FString::Printf(TEXT("after %s, a 1013 auth_unavailable retry keeps the credential and reconnects"), *Label),
			Subsystem->IsRealtimeReconnectScheduled());

		RefuseLiveReauth(Code);
		Subsystem->RealtimeSession->OnStateChanged(Haiven::ESessionState::Ready);
		CloseBy(1006, TEXT("network"));
		TestTrue(FString::Printf(TEXT("after %s, a retry that authenticated reconnects after an ordinary drop"), *Label),
			Subsystem->IsRealtimeReconnectScheduled());

		RefuseLiveReauth(Code);
		CloseBy(1008, TEXT("unauthorized"));
		TestFalse(FString::Printf(TEXT("after %s, a retry refused with 1008 unauthorized is final"), *Label),
			Subsystem->IsRealtimeReconnectScheduled());
		FString ClearedToken;
		TestFalse(FString::Printf(TEXT("after %s, the 1008 unauthorized retry clears the credential"), *Label),
			Subsystem->GetAccessToken(ClearedToken));
	}
	Subsystem->SetAccessToken(TEXT("player-b"), Expiry);

	Connect();
	Subsystem->RealtimeSession->OnClosed = [Subsystem](int, const std::string&) { Subsystem->DisconnectRealtime(); };
	CloseBy(1006, TEXT("network"));
	TestFalse(TEXT("a close callback that disconnects is not overridden"), Subsystem->IsRealtimeReconnectScheduled());

	Options.bReconnectRealtime = false;
	Subsystem->Configure(Options);
	Connect();
	CloseBy(1006, TEXT("network"));
	TestFalse(TEXT("bReconnectRealtime=false never reconnects"), Subsystem->IsRealtimeReconnectScheduled());

	Subsystem->DisconnectRealtime();
	Subsystem->ReleaseRetiredRealtimeSessions();
	return true;
}
#endif
