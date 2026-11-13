#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/GameInstance.h"
#include "Framework/Docking/TabManager.h"
#include "HaivenHealth.h"
#include "HaivenSubsystem.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Modules/ModuleManager.h"

namespace
{
	class FHaivenLogCapture final : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName& Category) override
		{
			if (Category == FName(TEXT("LogHaiven"))) Messages.Emplace(Message);
		}

		bool Contains(const FString& Text) const
		{
			return Messages.ContainsByPredicate(
				[&Text](const FString& Message) { return Message.Contains(Text); });
		}

	private:
		TArray<FString> Messages;
	};
}

// The subsystem's diagnostics: the Get Health snapshot a game's
// status UI polls, the failures it records with their area, and the LogHaiven
// warnings those failures emit. The URL-redaction check opens a socket only
// long enough to capture the lifecycle log, then disconnects without a payload.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenDiagnosticsHealth,
	"Haiven.Diagnostics.Health",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenDiagnosticsHealth::RunTest(const FString& Parameters)
{
	UFunction* GetHealth = UHaivenSubsystem::StaticClass()->FindFunctionByName(TEXT("GetHealth"));
	if (!TestNotNull(TEXT("Get Health is reflected"), GetHealth)) return false;
	TestTrue(TEXT("Get Health is a pure Blueprint node"), GetHealth->HasAnyFunctionFlags(FUNC_BlueprintPure));

	UHaivenSubsystem* Subsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());

	FHaivenHealth Health = Subsystem->GetHealth();
	TestEqual(TEXT("starts disconnected"), Health.RealtimeState, EHaivenRealtimeState::Disconnected);
	TestFalse(TEXT("starts without a token"), Health.bHasAccessToken);
	TestFalse(TEXT("starts without a token expiry"), Health.bHasAccessTokenExpiry);
	TestFalse(TEXT("starts without a login"), Health.bLoginInProgress);
	TestEqual(TEXT("starts without an error"), Health.LastErrorArea, EHaivenHealthArea::None);
	TestFalse(TEXT("starts without a close"), Health.bHasLastClose);
	TestEqual(TEXT("starts with no ready sessions"), Health.RealtimeReadyCount, 0);

	const FDateTime ExpiredAt = FDateTime::UtcNow() - FTimespan::FromMinutes(1);
	Subsystem->SetAccessToken(TEXT("expired"), ExpiredAt);
	TestFalse(TEXT("an expired token is not reported as available"), Subsystem->GetHealth().bHasAccessToken);
	Health = Subsystem->GetHealth();
	TestTrue(TEXT("an expired credential still reports its expiry"), Health.bHasAccessTokenExpiry);
	TestEqual(TEXT("the expired credential expiry is exact"), Health.AccessTokenExpiresAtUtc, ExpiredAt);
	const FDateTime LiveUntil = FDateTime::UtcNow() + FTimespan::FromHours(1);
	Subsystem->SetAccessToken(TEXT("live"), LiveUntil);
	Health = Subsystem->GetHealth();
	TestTrue(TEXT("an unexpired token is reported as available"), Health.bHasAccessToken);
	TestEqual(TEXT("the live credential expiry is exact"), Health.AccessTokenExpiresAtUtc, LiveUntil);

	for (int32 Index = 0; Index < 34; ++Index)
	{
		FString Type = TEXT("ping");
		if (Index == 0) Type = TEXT("auth");
		else if (Index == 1) Type = TEXT("pong");
		else if (Index == 2) Type = TEXT("auth.success");
		else if (Index == 33) Type = TEXT("chat.message");
		Subsystem->RecordRealtimeFrame(
			Index % 2 == 0 ? EHaivenRealtimeFrameDirection::Sent : EHaivenRealtimeFrameDirection::Received,
			Type);
	}
	Health = Subsystem->GetHealth();
	TestEqual(TEXT("recent frame metadata is bounded"), Health.RecentRealtimeFrames.Num(), 32);
	TestEqual(TEXT("the oldest retained frame type"), Health.RecentRealtimeFrames[0].Type, FString(TEXT("auth.success")));
	TestEqual(TEXT("a known frame type is retained"), Health.RecentRealtimeFrames.Last().Type, FString(TEXT("chat.message")));
	TestEqual(
		TEXT("frame direction is retained"),
		Health.RecentRealtimeFrames.Last().Direction,
		EHaivenRealtimeFrameDirection::Received);
	TestTrue(TEXT("frame metadata is timestamped"), Health.RecentRealtimeFrames.Last().SeenAtUtc > FDateTime());

	const FString SensitiveType = TEXT("Bearer diagnostic-secret private chat text");
	Subsystem->RecordRealtimeFrame(EHaivenRealtimeFrameDirection::Received, SensitiveType);
	Health = Subsystem->GetHealth();
	TestEqual(TEXT("a sensitive unknown type becomes an opaque marker"), Health.RecentRealtimeFrames.Last().Type, FString(TEXT("<unrecognized>")));
	TestFalse(TEXT("a sensitive unknown type is not retained"), Health.RecentRealtimeFrames.Last().Type.Contains(TEXT("diagnostic-secret")));

	FString OversizedType = FString::ChrN(64 * 1024, TCHAR('x'));
	OversizedType += SensitiveType;
	Subsystem->RecordRealtimeFrame(EHaivenRealtimeFrameDirection::Sent, OversizedType);
	Health = Subsystem->GetHealth();
	TestEqual(TEXT("an oversized unknown type becomes an opaque marker"), Health.RecentRealtimeFrames.Last().Type, FString(TEXT("<unrecognized>")));
	TestEqual(TEXT("adversarial types preserve the entry bound"), Health.RecentRealtimeFrames.Num(), 32);
	TestFalse(TEXT("an oversized sensitive type is not retained"), Health.RecentRealtimeFrames.Last().Type.Contains(TEXT("diagnostic-secret")));

	FHaivenClientOptions Options;
	Options.RealtimeUrl = TEXT("ws://example.com/ws");
	Subsystem->Configure(Options);
	AddExpectedMessage(TEXT("secure realtime URL"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	bool bConnected = true;
	Subsystem->ConnectRealtime(FHaivenConnectComplete::CreateLambda(
		[&bConnected](bool bSucceeded, const FString&) { bConnected = bSucceeded; }));
	TestFalse(TEXT("an insecure realtime URL is refused"), bConnected);
	Health = Subsystem->GetHealth();
	TestEqual(TEXT("the refusal is a realtime failure"), Health.LastErrorArea, EHaivenHealthArea::Realtime);
	TestTrue(TEXT("the refusal says why"), Health.LastError.Contains(TEXT("secure realtime URL")));
	TestTrue(TEXT("the refusal is timestamped"), Health.LastErrorAtUtc > FDateTime());
	TestEqual(TEXT("still disconnected"), Health.RealtimeState, EHaivenRealtimeState::Disconnected);
	TestEqual(TEXT("a refused connect is not a drop"), Health.RealtimeDropCount, 0);

	const FString UrlSecret = TEXT("diagnostic-secret");
	Options.RealtimeUrl = TEXT("wss://gateway.example/ws?api_key=") + UrlSecret;
	Subsystem->Configure(Options);
	FHaivenLogCapture LogCapture;
	GLog->AddOutputDevice(&LogCapture);
	Subsystem->ConnectRealtime();
	GLog->RemoveOutputDevice(&LogCapture);
	Subsystem->DisconnectRealtime();
	TestFalse(TEXT("realtime lifecycle logs omit URL credentials"), LogCapture.Contains(UrlSecret));

	AddExpectedMessage(TEXT("absolute base URL"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FHaivenHttpResult HttpResult;
	Subsystem->Request(TEXT("GET"), TEXT("/v1/me"), FString(), FHaivenHttpComplete::CreateLambda(
		[&HttpResult](const FHaivenHttpResult& Result) { HttpResult = Result; }));
	TestFalse(TEXT("a request without a base URL fails"), HttpResult.bSucceeded);
	Health = Subsystem->GetHealth();
	TestEqual(TEXT("the latest failure is HTTP"), Health.LastErrorArea, EHaivenHealthArea::Http);
	TestEqual(TEXT("no request is left pending"), Health.PendingHttpRequests, 0);
	TestFalse(TEXT("the snapshot never carries the token"), Health.LastError.Contains(TEXT("live")));

	AddExpectedMessage(TEXT("not connected"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	UHaivenSubsystem* DisconnectedSubsystem = NewObject<UHaivenSubsystem>(NewObject<UGameInstance>());
	FHaivenRealtimeReply DisconnectedReply;
	DisconnectedSubsystem->SendRealtimeRequest(TEXT("presence.set"), TEXT("{}"), FHaivenRealtimeComplete::CreateLambda(
		[&DisconnectedReply](const FHaivenRealtimeReply& Result) { DisconnectedReply = Result; }));
	TestFalse(TEXT("a disconnected realtime send fails"), DisconnectedReply.bSucceeded);
	const FHaivenHealth DisconnectedHealth = DisconnectedSubsystem->GetHealth();
	TestEqual(TEXT("a disconnected send records a realtime failure"), DisconnectedHealth.LastErrorArea, EHaivenHealthArea::Realtime);
	TestEqual(TEXT("the disconnected send is explained"), DisconnectedHealth.LastError, DisconnectedReply.Error);
	TestTrue(TEXT("the disconnected send is timestamped"), DisconnectedHealth.LastErrorAtUtc > FDateTime());

	const FString ReplyError = TEXT("gateway refused the realtime request");
	AddExpectedMessage(ReplyError, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FHaivenRealtimeReply FailedReply;
	Subsystem->RealtimeRequests.Add(TEXT("request-1"), FHaivenRealtimeComplete::CreateLambda(
		[&FailedReply](const FHaivenRealtimeReply& Result) { FailedReply = Result; }));
	Haiven::FServerFrame Frame;
	Frame.Type = "presence.set";
	Frame.RequestId = "request-1";
	Frame.Success = false;
	Frame.Error = "gateway refused the realtime request";
	Subsystem->HandleRealtimeReply(Frame, R"({"type":"presence.set","requestId":"request-1","success":false})");
	TestFalse(TEXT("an unsuccessful correlated reply fails"), FailedReply.bSucceeded);
	Health = Subsystem->GetHealth();
	TestEqual(TEXT("the correlated reply is a realtime failure"), Health.LastErrorArea, EHaivenHealthArea::Realtime);
	TestEqual(TEXT("the correlated reply replaces the previous HTTP error"), Health.LastError, ReplyError);
	TestEqual(TEXT("the correlated request is no longer pending"), Health.PendingRealtimeRequests, 0);

	TArray<FHaivenOutgoingChatMessage> Outbox;
	for (const EHaivenOutgoingChatState State : {
			 EHaivenOutgoingChatState::Sending, EHaivenOutgoingChatState::Sending,
			 EHaivenOutgoingChatState::Unconfirmed, EHaivenOutgoingChatState::Accepted,
			 EHaivenOutgoingChatState::Held, EHaivenOutgoingChatState::Removed,
			 EHaivenOutgoingChatState::Refused})
	{
		FHaivenOutgoingChatMessage& Message = Outbox.AddDefaulted_GetRef();
		Message.State = State;
	}
	FHaivenHealth Tally;
	Tally.TallyChatOutbox(Outbox);
	TestEqual(TEXT("sending"), Tally.ChatSending, 2);
	TestEqual(TEXT("unconfirmed"), Tally.ChatUnconfirmed, 1);
	TestEqual(TEXT("accepted"), Tally.ChatAccepted, 1);
	TestEqual(TEXT("held"), Tally.ChatHeld, 1);
	TestEqual(TEXT("removed"), Tally.ChatRemoved, 1);
	TestEqual(TEXT("refused"), Tally.ChatRefused, 1);
	return true;
}

// The editor module is part of the shipped plugin rather than the host. This
// catches descriptor/module regressions and proves its Window-menu tab spawner
// is registered on both exact engine heads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHaivenDiagnosticsEditorPanel,
	"Haiven.Diagnostics.EditorPanel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHaivenDiagnosticsEditorPanel::RunTest(const FString& Parameters)
{
	IModuleInterface* EditorModule = FModuleManager::Get().LoadModule(TEXT("HaivenEditor"));
	TestNotNull(TEXT("the Haiven editor module loads"), EditorModule);
	TestTrue(
		TEXT("the Haiven Diagnostics tab is registered"),
		FGlobalTabmanager::Get()->HasTabSpawner(FName(TEXT("Haiven.Diagnostics"))));
	return true;
}
#endif
